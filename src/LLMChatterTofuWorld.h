/*
 * mod-llm-chatter - Tofu Worldview addition: LLM chatter in the realm-wide
 * custom World channel.
 *
 * Bots anywhere on the realm (mostly bots outside the zones where real
 * players are) post ambient lines and short exchanges in the custom chat
 * channel named by LLMChatter.Tofu.World.ChannelName, and answer what real
 * players type there. Off unless LLMChatter.Tofu.World.Enable = 1.
 *
 * Flow: C++ queues tofu_world_ambient and tofu_world_player_msg events, the
 * bridge (tools/chatter_tofu_world.py) writes rows with channel 'world' to
 * llm_chatter_messages, and LLMChatterDelivery.cpp speaks them through
 * TofuWorld::Deliver().
 */

#ifndef MOD_LLM_CHATTER_TOFU_WORLD_H
#define MOD_LLM_CHATTER_TOFU_WORLD_H

#include <string>

class Player;

namespace TofuWorld
{
// True when the module and the World feature are both enabled.
bool IsActive();

// Speaks `text` in the World channel as `bot` and records it in the World
// history. On false, `dropReason` (if given) names why the line was dropped.
bool Deliver(Player* bot, std::string const& text, char const** dropReason);
}

void AddLLMChatterTofuWorldScripts();

#endif
