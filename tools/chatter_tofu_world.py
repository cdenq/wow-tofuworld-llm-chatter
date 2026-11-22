"""Tofu Worldview addition: LLM chatter in the realm-wide custom
World channel.

Handles the two events queued by src/LLMChatterTofuWorld.cpp:

- tofu_world_ambient: one bot (sometimes two) posts unprompted in
  World, written from the bot's own zone.
- tofu_world_player_msg: a bot answers what a real player typed in
  World. C++ supplies the candidates; a bot named by the player is
  first and always answers.

Rows are written with channel 'world' and delivered by
TofuWorld::Deliver() in C++, which also records the delivered line in
llm_tofu_world_history, the conversation context read here.
"""

import json
import logging
import random

from chatter_db import (
    get_bots_by_guid,
    get_race_name,
    insert_chat_message,
    mark_event,
)
from chatter_identity import prepare_channel_persona
from chatter_llm import call_llm
from chatter_mode import build_player_chat_guidance, is_roleplay
from chatter_persona import build_persona_block
from chatter_shared import (
    PromptParts,
    calculate_dynamic_delay,
    get_chatter_mode,
    get_class_name,
    get_zone_name,
    location_lore_lines,
)
from chatter_text import (
    cleanup_message,
    shorten_chat_message,
    strip_speaker_prefix,
)

logger = logging.getLogger(__name__)

OWNER_SUBSYSTEM = 'tofu_world'
CHANNEL = 'world'
HORDE_RACES = frozenset({2, 5, 6, 8, 10})
GENDERS = {0: 'male', 1: 'female'}


def _extra_data(event):
    extra = event.get('extra_data')
    if isinstance(extra, (str, bytes)):
        try:
            extra = json.loads(extra)
        except (TypeError, ValueError):
            return {}
    return extra if isinstance(extra, dict) else {}


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def _faction(race_id):
    return 'Horde' if _int(race_id) in HORDE_RACES else 'Alliance'


def _max_length(config):
    return max(40, min(_int(
        config.get('LLMChatter.MaxMessageLength', 250), 250), 250))


def _read_history(db, limit):
    """Recent World lines, oldest first."""
    cursor = db.cursor(dictionary=True)
    cursor.execute(
        "SELECT speaker_name, is_bot, team, message "
        "FROM llm_tofu_world_history "
        "ORDER BY id DESC LIMIT %s",
        (max(1, limit),),
    )
    rows = cursor.fetchall()
    cursor.close()
    rows.reverse()
    return rows


def _format_history(rows):
    if not rows:
        return "(the channel has been quiet)"
    return "\n".join(
        f"[{'Horde' if _int(r.get('team')) else 'Alliance'}] "
        f"{r.get('speaker_name', '?')}: {r.get('message', '')}"
        for r in rows
    )


def _system_block(config, mode, channel_name):
    max_len = _max_length(config)
    guidance = build_player_chat_guidance(mode, 'general')
    return (
        "You write one chat message for a character in World of "
        "Warcraft: Wrath of the Lich King (patch 3.3.5a). The message "
        f"is posted in \"{channel_name}\", a custom chat channel that "
        "every player on the realm reads wherever they are. Alliance "
        "and Horde share it; cross-faction talk there is ordinary and "
        "civil, between people who are usually far apart.\n"
        f"{guidance}\n"
        "Rules:\n"
        "- Output only the message text: no speaker name, no quotes, "
        "no stage directions, no asterisks, no emojis, no markdown.\n"
        "- One chat line. Length follows its purpose: often a few "
        "words, sometimes one or two sentences; never more than "
        f"{max_len} characters.\n"
        "- Only Wrath of the Lich King era knowledge. Do not invent "
        "live facts: what other players did, realm events, or lines "
        "that are not in the chat log.\n"
        "- Never mention being an AI, a bot, or a language model."
    )


def _speaker_card(db, client, config, mode, bot):
    guid = _int(bot['bot1_guid'])
    name = bot['bot1_name']
    persona = prepare_channel_persona(
        db, client, config, guid, name, 'general',
    )
    zone_id = _int(bot.get('zone_id'))
    zone = get_zone_name(zone_id) or 'the wilds'
    gender = GENDERS.get(_int(bot.get('bot1_gender'), -1), '')
    who = ' '.join(part for part in (
        f"a level {_int(bot.get('bot1_level'))}",
        gender,
        get_race_name(_int(bot.get('bot1_race'))),
        get_class_name(_int(bot.get('bot1_class'))),
    ) if part)
    lines = [
        f"You are {name}, {who} of the "
        f"{_faction(bot.get('bot1_race'))}.",
        f"You are currently in {zone}.",
    ]
    if is_roleplay(mode):
        lines.extend(location_lore_lines(zone_id, 0))
    persona_block = build_persona_block(persona, mode)
    if persona_block:
        lines.append(persona_block)
    return "\n".join(lines)


def _generate(client, config, mode, channel_name, user_prompt, label):
    prompt = PromptParts(
        user_prompt, _system_block(config, mode, channel_name),
    )
    raw = call_llm(
        client, prompt, config,
        max_tokens_override=200,
        context=label, label=label, free_text=True,
    )
    return raw


def _clean(raw, bot_name, config):
    if not raw:
        return ''
    text = cleanup_message(raw.strip())
    text = strip_speaker_prefix(text, bot_name)
    text = text.strip().strip('"').strip()
    return shorten_chat_message(text, _max_length(config))


def _fail(db, event, reason):
    logger.info(
        "tofu_world event %s (%s) skipped: %s",
        event.get('id'), event.get('event_type'), reason,
    )
    mark_event(db, event['id'], 'skipped')
    return False


def process_tofu_world_ambient_event(db, client, config, event):
    """A bot posts unprompted in World; sometimes a second bot
    answers it."""
    extra = _extra_data(event)
    channel_name = extra.get('channel_name') or 'World'
    mode = get_chatter_mode(config)
    history_limit = _int(extra.get('history_limit'), 12)

    guids = [_int(event.get('subject_guid'))]
    if _int(extra.get('conversation')) and _int(event.get('target_guid')):
        guids.append(_int(event.get('target_guid')))
    cursor = db.cursor(dictionary=True)
    bots = get_bots_by_guid(cursor, [g for g in guids if g])
    cursor.close()
    if not bots:
        return _fail(db, event, 'speaker offline')

    history = _read_history(db, history_limit)
    first = bots[0]
    first_card = _speaker_card(db, client, config, mode, first)
    zone = get_zone_name(_int(first.get('zone_id'))) or 'the wilds'
    user_prompt = (
        f"{first_card}\n\n"
        f"Recent {channel_name} chat (oldest first):\n"
        f"{_format_history(history)}\n\n"
        f"Write a new message you would post in {channel_name} right "
        "now, unprompted, from where you are. Let your personality "
        "pick it: what you are doing in "
        f"{zone}, a question for the realm, looking for help or a "
        "group, an opinion, a passing thought, or an answer to a "
        "recent line if one invites it. Do not repeat a topic or "
        "phrasing from the log."
    )
    line1 = _clean(
        _generate(client, config, mode, channel_name, user_prompt,
                  'tofu_world_ambient'),
        first['bot1_name'], config,
    )
    if not line1:
        return _fail(db, event, 'empty generation')

    delay1 = random.uniform(1.0, 4.0)
    insert_chat_message(
        db, _int(first['bot1_guid']), first['bot1_name'], line1,
        channel=CHANNEL, delay_seconds=delay1,
        event_id=event['id'], sequence=0, config=config,
        owner_subsystem=OWNER_SUBSYSTEM,
    )

    if len(bots) > 1:
        partner = bots[1]
        partner_card = _speaker_card(
            db, client, config, mode, partner,
        )
        reply_prompt = (
            f"{partner_card}\n\n"
            f"Recent {channel_name} chat (oldest first):\n"
            f"{_format_history(history)}\n"
            f"[{_faction(first.get('bot1_race'))}] "
            f"{first['bot1_name']}: {line1}\n\n"
            f"{first['bot1_name']} just wrote that in {channel_name}. "
            "Reply to what they actually said, in your own voice: "
            "agree, disagree, answer, tease, or ask something back. "
            "A few words are fine."
        )
        line2 = _clean(
            _generate(client, config, mode, channel_name,
                      reply_prompt, 'tofu_world_ambient_reply'),
            partner['bot1_name'], config,
        )
        if line2:
            delay2 = delay1 + calculate_dynamic_delay(
                len(line2), config, prev_message_length=len(line1),
            )
            insert_chat_message(
                db, _int(partner['bot1_guid']), partner['bot1_name'],
                line2, channel=CHANNEL, delay_seconds=delay2,
                event_id=event['id'], sequence=1, config=config,
                owner_subsystem=OWNER_SUBSYSTEM,
            )

    mark_event(db, event['id'], 'completed')
    return True


def process_tofu_world_player_msg_event(db, client, config, event):
    """A bot answers what a real player typed in World."""
    extra = _extra_data(event)
    channel_name = extra.get('channel_name') or 'World'
    mode = get_chatter_mode(config)
    history_limit = _int(extra.get('history_limit'), 12)
    player_name = extra.get('player_name') or event.get('subject_name')
    player_message = (extra.get('player_message') or '').strip()
    if not player_name or not player_message:
        return _fail(db, event, 'missing player message')

    candidates = [
        _int(g) for g in (extra.get('bot_guids') or []) if _int(g)
    ]
    cursor = db.cursor(dictionary=True)
    bots = get_bots_by_guid(cursor, candidates)
    cursor.close()
    if not bots:
        return _fail(db, event, 'no candidate online')

    mentioned = bool(_int(extra.get('mentioned')))
    mentioned_guid = _int(event.get('target_guid'))
    speaker = None
    if mentioned:
        speaker = next(
            (b for b in bots if _int(b['bot1_guid']) == mentioned_guid),
            None,
        )
    if speaker is None:
        speaker = random.choice(bots)

    history = _read_history(db, history_limit)
    # C++ recorded the player's line before queueing; it is quoted
    # separately below.
    if history and not _int(history[-1].get('is_bot')) \
            and history[-1].get('message') == player_message:
        history = history[:-1]

    player_who = ' '.join(part for part in (
        f"level {_int(extra.get('player_level'))}",
        get_race_name(_int(extra.get('player_race'))),
        get_class_name(_int(extra.get('player_class'))),
    ) if part)
    player_team = extra.get('player_team') or 'Alliance'
    player_zone = get_zone_name(_int(extra.get('player_zone_id')))
    where = f", in {player_zone}" if player_zone else ''
    addressed = (
        " They addressed you by name." if mentioned else ''
    )
    card = _speaker_card(db, client, config, mode, speaker)
    user_prompt = (
        f"{card}\n\n"
        f"Recent {channel_name} chat (oldest first):\n"
        f"{_format_history(history)}\n\n"
        f"{player_name} ({player_who}, {player_team}{where}) just "
        f"wrote in {channel_name}: \"{player_message}\"{addressed}\n\n"
        f"Reply to {player_name} in {channel_name}. Respond to what "
        "they actually said: answer, help, joke, disagree, or ask "
        "something back, as your personality would. Leave their "
        "decisions, feelings, and actions to them."
    )
    line = _clean(
        _generate(client, config, mode, channel_name, user_prompt,
                  'tofu_world_player_reply'),
        speaker['bot1_name'], config,
    )
    if not line:
        return _fail(db, event, 'empty generation')

    insert_chat_message(
        db, _int(speaker['bot1_guid']), speaker['bot1_name'], line,
        channel=CHANNEL,
        delay_seconds=calculate_dynamic_delay(
            len(line), config, responsive=True,
        ),
        event_id=event['id'], sequence=0, config=config,
        owner_subsystem=OWNER_SUBSYSTEM,
    )
    mark_event(db, event['id'], 'completed')
    return True
