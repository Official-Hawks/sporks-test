/************************************************************************************
 * 
 * Sporks, the learning, scriptable Discord bot!
 *
 * Copyright 2019 Craig Edwards <support@sporks.gg>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ************************************************************************************/

#include "infobot.h"
#include <sporks/bot.h>
#include <sporks/includes.h>
#include "queue.h"
#include <sporks/config.h>
#include <sporks/stringops.h>
#include <sporks/modules.h>
#include <iostream>
#include <sstream>
#include <thread>
#include "backend.h"

int InfobotModule::random(int min, int max)
{
	static bool first = true;
	if (first) {
		srand(time(NULL));
		first = false;
	}
	return min + rand() % (( max + 1 ) - min);
}

QueueStats InfobotModule::GetQueueStats() {
	QueueStats q;

	q.inputs = inputs.size();
	q.outputs = outputs.size();
	q.users = 0;
	if (bot->counters.find("userqueue") != bot->counters.end()) {
		q.users = bot->counters["userqueue"];
	}

	return q;
}

void InfobotModule::InputThread()
{
	while (!this->terminate) {
		try {
			while (true) {
				/* Process anything in the inputs queue */
				QueueItem query;
				bool has_item = false;
				/* Block to encapsulate lock_guard for input queue */
				do {
					std::lock_guard<std::mutex> input_lock(this->input_mutex);
					if (!inputs.empty()) {
						query = inputs.front();
						json channel_settings;
						do {
							std::lock_guard<std::mutex> hash_lock(bot->channel_hash_mutex);
							channel_settings = getSettings(bot, query.channelID, query.serverID);
						} while(false);

						/* Process the input through to infobot backend if:
						 * A) the bot is directly mentioned, or,
						 * B) Learning is enabled for the channel (default for all channels)
						 */
						has_item = query.mentioned || settings::IsLearningEnabled(channel_settings);
						inputs.pop_front();
					}
				} while(false);
				if (has_item) {
					/* Fix: If there isnt a list yet, don't try and do this otherwise it will result in a call of random(0, -1) and a SIGFPE */
					std::string randnick = "";
					if (nickList.find(query.serverID) != nickList.end() && this->nickList[query.serverID].size() > 0) {
						randnick =  this->nickList[query.serverID][random(0, this->nickList[query.serverID].size() - 1)];
					}

					/* Mangle common prefixes, so if someone asks "What is x" it is treated same as "x?" */
					std::string cleaned_message = query.message;
					std::vector<std::string> prefixes = {
						"what is a",
						"whats",
						"whos",
						"whats up with",
						"whats going off with",
						"what is",
						"tell me about",
						"who is",
						"what are",
						"who are",
						"wtf is",
						"tell me about",
						"tell me",
						"can someone help me with",
						"can you help me with",
						"can you help me",
						"can someone help me",
						"can i ask about",
						"can i ask",
						"do you",
						"can you",
						"will you",
						"wont you",
						"won't you",
						"how do i",
					};
					for (auto p = prefixes.begin(); p != prefixes.end(); ++p) {
						if (lowercase(trim(cleaned_message.substr(0, p->length()))) == *p) {
							cleaned_message = trim(cleaned_message.substr(p->length(), cleaned_message.length() - p->length()));
						}
					}

					infodef def;
					std::string text = infobot_response(bot->user.username, cleaned_message, query.username, randnick, query.channelID, def);
					bool found = def.found;
		
					if ((found || query.mentioned) && query.tombstone == false) {
						QueueItem resp;
						resp.username = query.username;
						resp.tombstone = query.tombstone;
						resp.message = text;
						resp.channelID = query.channelID;
						resp.serverID = query.serverID;
						resp.mentioned = query.mentioned;
						{
							std::lock_guard<std::mutex> output_lock(this->output_mutex);
							outputs.push_back(resp);
						};
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				} else {
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
				}
			}
		}
		catch (const std::exception &e) {
			bot->core.log->error("Infobot socket: caught connection exception: {}", e.what());
		}
		std::this_thread::sleep_for(std::chrono::seconds(5));
	}
}

InfobotModule::InfobotModule(Bot* instigator, ModuleLoader* ml) : Module(instigator, ml), terminate(false), thr_input(nullptr), thr_output(nullptr)
{
	ml->Attach({ I_OnMessage, I_OnGuildCreate, I_OnGuildDelete }, this);

	infobot_init();

	thr_input = new std::thread(&InfobotModule::InputThread, this);
	thr_output = new std::thread(&InfobotModule::OutputThread, this);	
}

InfobotModule::~InfobotModule()
{
	terminate = true;
	bot->DisposeThread(thr_input);
	bot->DisposeThread(thr_output);
}

std::string InfobotModule::GetVersion()
{
	/* NOTE: This version string below is modified by a pre-commit hook on the git repository */
	std::string version = "$ModVer 10$";
	return "1.0." + version.substr(8,version.length() - 9);
}

std::string InfobotModule::GetDescription()
{
	return "Infobot learning and responses";
}

bool InfobotModule::OnGuildCreate(const modevent::guild_create &gc)
{
	this->nickList[gc.guild.id.get()] = std::vector<std::string>();
	for (auto i = gc.guild.members.begin(); i != gc.guild.members.end(); ++i) {
		this->nickList[gc.guild.id.get()].push_back(i->_user.username);
	}
	return true;
}

bool InfobotModule::OnGuildDelete(const modevent::guild_delete &guild)
{
	// Clear any queue items for a server that no longer exists
	std::lock_guard<std::mutex> input_lock(input_mutex);
	std::lock_guard<std::mutex> output_lock(this->output_mutex);
	for (auto i = inputs.begin(); i != inputs.end(); ++i) {
		if (i->serverID == guild.guild_id.get()) {
			i->tombstone = true;
		}
	}
	for (auto i = outputs.begin(); i != outputs.end(); ++i) {
		if (i->serverID == guild.guild_id.get()) {
			i->tombstone = true; 
		}
	}
	return true;
}

bool InfobotModule::OnMessage(const modevent::message_create &message, const std::string& clean_message, bool mentioned, const std::vector<std::string> &stringmentions)
{
	modevent::message_create msg = message;

	QueueItem query;
	query.tombstone = false;
	query.message = clean_message;
	query.channelID = msg.channel.get_id().get();
	query.serverID = msg.msg.get_guild_id().get();
	query.username = msg.msg.get_user().get_username();
	query.mentioned = mentioned;

	std::lock_guard<std::mutex> input_lock(input_mutex);
	inputs.push_back(query);

	return true;
}

ENTRYPOINT(InfobotModule);

