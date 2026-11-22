"""Tofu World channel handlers (chatter_tofu_world)."""

import json

import pytest

import chatter_tofu_world as tw
from chatter_persona import Persona


class _Cursor:
    def close(self):
        pass


class _Db:
    def cursor(self, dictionary=False):
        return _Cursor()


def _bot(guid, name, race=1, zone=40):
    return {
        'bot1_guid': guid, 'bot1_name': name, 'bot1_class': 1,
        'bot1_race': race, 'bot1_level': 30, 'bot1_gender': 0,
        'zone_id': zone,
    }


@pytest.fixture
def harness(monkeypatch):
    state = {'inserts': [], 'marks': [], 'prompts': [], 'replies': []}

    monkeypatch.setattr(tw, '_read_history', lambda db, limit: [])
    monkeypatch.setattr(
        tw, 'prepare_channel_persona',
        lambda db, client, config, guid, name, channel: Persona(
            name=name, traits=('curious',), tone='dry'),
    )

    def fake_llm(client, prompt, config, **kwargs):
        state['prompts'].append(prompt)
        return state['replies'].pop(0) if state['replies'] else ''

    monkeypatch.setattr(tw, 'call_llm', fake_llm)
    monkeypatch.setattr(
        tw, 'insert_chat_message',
        lambda db, guid, name, msg, **kw: state['inserts'].append(
            dict(guid=guid, name=name, msg=msg, **kw)),
    )
    monkeypatch.setattr(
        tw, 'mark_event',
        lambda db, event_id, status: state['marks'].append(status),
    )
    return state


def test_ambient_conversation_writes_two_world_lines(harness, monkeypatch):
    monkeypatch.setattr(
        tw, 'get_bots_by_guid',
        lambda cursor, guids: [_bot(11, 'Aldo'), _bot(12, 'Grosh', race=2)],
    )
    harness['replies'] = ['anyone up for Gnomeregan?', 'not with you lol']
    event = {
        'id': 5, 'event_type': 'tofu_world_ambient',
        'subject_guid': 11, 'target_guid': 12,
        'extra_data': json.dumps({
            'channel_name': 'World', 'conversation': 1,
            'speaker_zone_id': 40, 'history_limit': 12,
        }),
    }

    assert tw.process_tofu_world_ambient_event(_Db(), None, {}, event)
    assert [i['channel'] for i in harness['inserts']] == ['world', 'world']
    assert [i['sequence'] for i in harness['inserts']] == [0, 1]
    assert {i['owner_subsystem'] for i in harness['inserts']} == {
        'tofu_world'}
    assert harness['inserts'][1]['delay_seconds'] > \
        harness['inserts'][0]['delay_seconds']
    assert 'Aldo: anyone up for Gnomeregan?' in \
        harness['prompts'][1].user_prompt
    assert harness['marks'] == ['completed']


def test_player_reply_uses_the_named_bot(harness, monkeypatch):
    monkeypatch.setattr(
        tw, 'get_bots_by_guid',
        lambda cursor, guids: [_bot(21, 'Mira'), _bot(22, 'Tolk')],
    )
    harness['replies'] = ['Tolk: right here, what do you need?']
    event = {
        'id': 6, 'event_type': 'tofu_world_player_msg',
        'subject_guid': 1, 'subject_name': 'Chris', 'target_guid': 22,
        'extra_data': json.dumps({
            'channel_name': 'World', 'player_name': 'Chris',
            'player_message': 'Tolk where are you?', 'mentioned': 1,
            'bot_guids': [22, 21], 'player_race': 1, 'player_class': 2,
            'player_level': 12, 'player_team': 'Alliance',
            'player_zone_id': 12,
        }),
    }

    assert tw.process_tofu_world_player_msg_event(_Db(), None, {}, event)
    insert = harness['inserts'][0]
    assert insert['guid'] == 22
    assert insert['channel'] == 'world'
    assert not insert['msg'].startswith('Tolk:')
    assert 'They addressed you by name.' in \
        harness['prompts'][0].user_prompt
    assert harness['marks'] == ['completed']


def test_empty_generation_skips_the_event(harness, monkeypatch):
    monkeypatch.setattr(
        tw, 'get_bots_by_guid', lambda cursor, guids: [_bot(11, 'Aldo')],
    )
    event = {
        'id': 7, 'event_type': 'tofu_world_ambient',
        'subject_guid': 11, 'target_guid': 0,
        'extra_data': {'channel_name': 'World', 'conversation': 0},
    }

    assert not tw.process_tofu_world_ambient_event(_Db(), None, {}, event)
    assert harness['inserts'] == []
    assert harness['marks'] == ['skipped']


def test_offline_speaker_skips_the_event(harness, monkeypatch):
    monkeypatch.setattr(tw, 'get_bots_by_guid', lambda cursor, guids: [])
    event = {
        'id': 8, 'event_type': 'tofu_world_player_msg',
        'subject_name': 'Chris',
        'extra_data': {'player_name': 'Chris', 'player_message': 'hi',
                       'bot_guids': [9]},
    }

    assert not tw.process_tofu_world_player_msg_event(_Db(), None, {}, event)
    assert harness['marks'] == ['skipped']
