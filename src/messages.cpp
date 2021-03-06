/* Common message handlers
 *
 * (C) 2003-2012 Anope Team
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 *
 */

#include "services.h"
#include "modules.h"
#include "users.h"
#include "protocol.h"
#include "config.h"
#include "uplink.h"
#include "opertype.h"
#include "messages.h"
#include "servers.h"
#include "channels.h"

using namespace Message;

void Away::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	FOREACH_MOD(I_OnUserAway, OnUserAway(source.GetUser(), params.empty() ? "" : params[0]));
}

void Capab::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	if (params.size() == 1)
	{
		spacesepstream sep(params[0]);
		Anope::string token;
		while (sep.GetToken(token))
			Servers::Capab.insert(token);
	}
	else
		for (unsigned i = 0; i < params.size(); ++i)
			Servers::Capab.insert(params[i]);
}

void Error::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	Log(LOG_TERMINAL) << "ERROR: " << params[0];
	Anope::QuitReason = "Received ERROR from uplink: " + params[0];
	Anope::Quitting = true;
}

void Join::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	User *user = source.GetUser();
	const Anope::string &channels = params[0];

	Anope::string channel;
	commasepstream sep(channels);

	while (sep.GetToken(channel))
	{
		/* Special case for /join 0 */
		if (channel == "0")
		{
			for (UChannelList::iterator it = user->chans.begin(), it_end = user->chans.end(); it != it_end; )
			{
				ChannelContainer *cc = *it++;

				Anope::string channame = cc->chan->name;
				FOREACH_MOD(I_OnPrePartChannel, OnPrePartChannel(user, cc->chan));
				cc->chan->DeleteUser(user);
				FOREACH_MOD(I_OnPartChannel, OnPartChannel(user, Channel::Find(channame), channame, ""));
			}
			user->chans.clear();
			continue;
		}

		std::list<SJoinUser> users;
		users.push_back(std::make_pair(ChannelStatus(), user));

		Channel *chan = Channel::Find(channel);
		SJoin(source, channel, chan ? chan->creation_time : Anope::CurTime, "", users);
	}
}

void Join::SJoin(MessageSource &source, const Anope::string &chan, time_t ts, const Anope::string &modes, const std::list<SJoinUser> &users)
{
	Channel *c = Channel::Find(chan);
	bool keep_their_modes = true;

	if (!c)
	{
		c = new Channel(chan, ts ? ts : Anope::CurTime);
		c->SetFlag(CH_SYNCING);
	}
	/* Some IRCds do not include a TS */
	else if (!ts)
		;
	/* Our creation time is newer than what the server gave us, so reset the channel to the older time */
	else if (c->creation_time > ts)
	{
		c->creation_time = ts;
		c->Reset();
	}
	/* Their TS is newer, don't accept any modes from them */
	else if (ts > c->creation_time)
		keep_their_modes = false;
	
	/* Update the modes for the channel */
	if (keep_their_modes && !modes.empty())
		c->SetModesInternal(source, modes);
	
	for (std::list<SJoinUser>::const_iterator it = users.begin(), it_end = users.end(); it != it_end; ++it)
	{
		const ChannelStatus &status = it->first;
		User *u = it->second;

		EventReturn MOD_RESULT;
		FOREACH_RESULT(I_OnPreJoinChannel, OnPreJoinChannel(u, c));

		/* Add the user to the channel */
		UserContainer *cc = c->JoinUser(u);

		/* Update their status internally on the channel */
		*cc->status = status;

		/* Set whatever modes the user should have, and remove any that
		 * they aren't allowed to have (secureops etc).
		 */
		c->SetCorrectModes(u, true, true);

		/* Check to see if modules want the user to join, if they do
		 * check to see if they are allowed to join (CheckKick will kick/ban them
		 * if they aren't).
		 */
		if (MOD_RESULT != EVENT_STOP && c->ci && c->ci->CheckKick(u))
			continue;

		FOREACH_MOD(I_OnJoinChannel, OnJoinChannel(u, c));
	}

	/* Channel is done syncing */
	if (c->HasFlag(CH_SYNCING))
	{
		c->UnsetFlag(CH_SYNCING);
		/* Sync the channel (mode lock, topic, etc) */
		c->Sync();
	}
}

void Kick::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	const Anope::string &channel = params[0];
	const Anope::string &users = params[1];
	const Anope::string &reason = params.size() > 2 ? params[2] : "";

	Channel *c = Channel::Find(channel);
	if (!c)
		return;

	Anope::string user;
	commasepstream sep(users);

	while (sep.GetToken(user))
		c->KickInternal(source, user, reason);
}

void Kill::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	User *u = User::Find(params[0]);
	BotInfo *bi;

	if (!u)
		return;

	/* Recover if someone kills us. */
	if (u->server == Me && (bi = dynamic_cast<BotInfo *>(u)))
	{
		static time_t last_time = 0;

		if (last_time == Anope::CurTime)
		{
			Anope::QuitReason = "Kill loop detected. Are Services U:Lined?";
			Anope::Quitting = true;
			return;
		}
		last_time = Anope::CurTime;

		bi->introduced = false;
		IRCD->SendClientIntroduction(bi);
		bi->introduced = true;

		for (UChannelList::const_iterator cit = bi->chans.begin(), cit_end = bi->chans.end(); cit != cit_end; ++cit)
			IRCD->SendJoin(bi, (*cit)->chan, (*cit)->status);
	}
	else
		u->KillInternal(source.GetSource(), params[1]);
}

void Message::Mode::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	if (IRCD->IsChannelValid(params[0]))
	{
		Channel *c = Channel::Find(params[0]);

		if (c)
			c->SetModesInternal(source, params[2], 0);
	}
	else
	{
		User *u = User::Find(params[0]);

		if (u)
			u->SetModesInternal("%s", params[1].c_str());
	}
}

/* XXX We should cache the file somewhere not open/read/close it on every request */
void MOTD::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	Server *s = Server::Find(params[0]);
	if (s != Me)
		return;

	FILE *f = fopen(Config->MOTDFilename.c_str(), "r");
	if (f)
	{
		IRCD->SendNumeric(375, source.GetSource(), ":- %s Message of the Day", Config->ServerName.c_str());
		char buf[BUFSIZE];
		while (fgets(buf, sizeof(buf), f))
		{
			buf[strlen(buf) - 1] = 0;
			IRCD->SendNumeric(372, source.GetSource(), ":- %s", buf);
		}
		fclose(f);
		IRCD->SendNumeric(376, source.GetSource(), ":End of /MOTD command.");
	}
	else
		IRCD->SendNumeric(422, source.GetSource(), ":- MOTD file not found!  Please contact your IRC administrator.");
}

void Part::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	User *u = source.GetUser();
	const Anope::string &reason = params.size() > 1 ? params[1] : "";

	Anope::string channel;
	commasepstream sep(params[0]);

	while (sep.GetToken(channel))
	{
		Reference<Channel> c = Channel::Find(channel);

		if (!c || !u->FindChannel(c))
			continue;

		Log(u, c, "part") << "Reason: " << (!reason.empty() ? reason : "No reason");
		FOREACH_MOD(I_OnPrePartChannel, OnPrePartChannel(u, c));
		Anope::string ChannelName = c->name;
		c->DeleteUser(u);
		FOREACH_MOD(I_OnPartChannel, OnPartChannel(u, c, ChannelName, !reason.empty() ? reason : ""));
	}
}

void Ping::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	IRCD->SendPong(params.size() > 1 ? params[1] : Me->GetSID(), params[0]);
}

void Privmsg::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	const Anope::string &receiver = params[0];
	Anope::string message = params[1];

	User *u = source.GetUser();

	if (IRCD->IsChannelValid(receiver))
	{
		Channel *c = Channel::Find(receiver);
		if (c)
		{
			FOREACH_MOD(I_OnPrivmsg, OnPrivmsg(u, c, message));
		}
	}
	else
	{
		/* If a server is specified (nick@server format), make sure it matches
		 * us, and strip it off. */
		Anope::string botname = receiver;
		size_t s = receiver.find('@');
		if (s != Anope::string::npos)
		{
			Anope::string servername(receiver.begin() + s + 1, receiver.end());
			botname = botname.substr(0, s);
			if (!servername.equals_ci(Config->ServerName))
				return;
		}
		else if (Config->UseStrictPrivMsg)
		{
			const BotInfo *bi = BotInfo::Find(receiver);
			if (!bi)
				return;
			Log(LOG_DEBUG) << "Ignored PRIVMSG without @ from " << u->nick;
			u->SendMessage(bi, _("\"/msg %s\" is no longer supported.  Use \"/msg %s@%s\" or \"/%s\" instead."), bi->nick.c_str(), bi->nick.c_str(), Config->ServerName.c_str(), bi->nick.c_str());
			return;
		}

		BotInfo *bi = BotInfo::Find(botname);

		if (bi)
		{
			EventReturn MOD_RESULT;
			FOREACH_RESULT(I_OnBotPrivmsg, OnBotPrivmsg(u, bi, message));
			if (MOD_RESULT == EVENT_STOP)
				return;

			if (message[0] == '\1' && message[message.length() - 1] == '\1')
			{
				if (message.substr(0, 6).equals_ci("\1PING "))
				{
					Anope::string buf = message;
					buf.erase(buf.begin());
					buf.erase(buf.end() - 1);
					IRCD->SendCTCP(bi, u->nick, "%s", buf.c_str());
				}
				else if (message.substr(0, 9).equals_ci("\1VERSION\1"))
				{
					Module *enc = ModuleManager::FindFirstOf(ENCRYPTION);
					IRCD->SendCTCP(bi, u->nick, "VERSION Anope-%s %s :%s - (%s) -- %s", Anope::Version().c_str(), Config->ServerName.c_str(), IRCD->GetProtocolName().c_str(), enc ? enc->name.c_str() : "(none)", Anope::VersionBuildString().c_str());
				}
				return;
			}
			
			bi->OnMessage(u, message);
		}
	}

	return;
}

void Quit::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	const Anope::string &reason = params[0];
	User *user = source.GetUser();

	Log(user, "quit") << "quit (Reason: " << (!reason.empty() ? reason : "no reason") << ")";

	NickAlias *na = NickAlias::Find(user->nick);
	if (na && !na->nc->HasFlag(NI_SUSPENDED) && (user->IsRecognized() || user->IsIdentified(true)))
	{
		na->last_seen = Anope::CurTime;
		na->last_quit = reason;
	}
	FOREACH_MOD(I_OnUserQuit, OnUserQuit(user, reason));
	delete user;

	return;
}

void SQuit::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	Server *s = Server::Find(params[0]);

	if (!s)
	{
		Log() << "SQUIT for nonexistent server " << params[0];
		return;
	}

	FOREACH_MOD(I_OnServerQuit, OnServerQuit(s));

	s->Delete(s->GetName() + " " + s->GetUplink()->GetName());

	return;
}

void Stats::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	User *u = source.GetUser();

	switch (params[0][0])
	{
		case 'l':
			if (u->HasMode(UMODE_OPER))
			{
				IRCD->SendNumeric(211, source.GetSource(), "Server SendBuf SentBytes SentMsgs RecvBuf RecvBytes RecvMsgs ConnTime");
				IRCD->SendNumeric(211, source.GetSource(), "%s %d %d %d %d %d %d %ld", Config->Uplinks[Anope::CurrentUplink]->host.c_str(), UplinkSock->WriteBufferLen(), TotalWritten, -1, UplinkSock->ReadBufferLen(), TotalRead, -1, static_cast<long>(Anope::CurTime - Anope::StartTime));
			}

			IRCD->SendNumeric(219, source.GetSource(), "%c :End of /STATS report.", params[0][0]);
			break;
		case 'o':
		case 'O':
			/* Check whether the user is an operator */
			if (!u->HasMode(UMODE_OPER) && Config->HideStatsO)
				IRCD->SendNumeric(219, source.GetSource(), "%c :End of /STATS report.", params[0][0]);
			else
			{
				for (unsigned i = 0; i < Config->Opers.size(); ++i)
				{
					Oper *o = Config->Opers[i];

					const NickAlias *na = NickAlias::Find(o->name);
					if (na)
						IRCD->SendNumeric(243, source.GetSource(), "O * * %s %s 0", o->name.c_str(), o->ot->GetName().c_str());
				}

				IRCD->SendNumeric(219, source.GetSource(), "%c :End of /STATS report.", params[0][0]);
			}

			break;
		case 'u':
		{
			time_t uptime = Anope::CurTime - Anope::StartTime;
			IRCD->SendNumeric(242, source.GetSource(), ":Services up %d day%s, %02d:%02d:%02d", uptime / 86400, uptime / 86400 == 1 ? "" : "s", (uptime / 3600) % 24, (uptime / 60) % 60, uptime % 60);
			IRCD->SendNumeric(250, source.GetSource(), ":Current users: %d (%d ops); maximum %d", UserListByNick.size(), OperCount, MaxUserCount);
			IRCD->SendNumeric(219, source.GetSource(), "%c :End of /STATS report.", params[0][0]);
			break;
		} /* case 'u' */

		default:
			IRCD->SendNumeric(219, source.GetSource(), "%c :End of /STATS report.", params[0][0]);
	}

	return;
}

void Time::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	time_t t;
	time(&t);
	struct tm *tm = localtime(&t);
	char buf[64];
	strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y %Z", tm);
	IRCD->SendNumeric(391, source.GetSource(), "%s :%s", Config->ServerName.c_str(), buf);
	return;
}

void Topic::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	Channel *c = Channel::Find(params[0]);
	if (c)
		c->ChangeTopicInternal(source.GetSource(), params[1], Anope::CurTime);

	return;
}

void Version::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	Module *enc = ModuleManager::FindFirstOf(ENCRYPTION);
	IRCD->SendNumeric(351, source.GetSource(), "Anope-%s %s :%s -(%s) -- %s", Anope::Version().c_str(), Config->ServerName.c_str(), IRCD->GetProtocolName().c_str(), enc ? enc->name.c_str() : "(none)", Anope::VersionBuildString().c_str());
	return;
}

void Whois::Run(MessageSource &source, const std::vector<Anope::string> &params)
{
	User *u = User::Find(params[0]);

	if (u && u->server == Me)
	{
		const BotInfo *bi = BotInfo::Find(u->nick);
		IRCD->SendNumeric(311, source.GetSource(), "%s %s %s * :%s", u->nick.c_str(), u->GetIdent().c_str(), u->host.c_str(), u->realname.c_str());
		if (bi)
			IRCD->SendNumeric(307, source.GetSource(), "%s :is a registered nick", bi->nick.c_str());
		IRCD->SendNumeric(312, source.GetSource(), "%s %s :%s", u->nick.c_str(), Config->ServerName.c_str(), Config->ServerDesc.c_str());
		if (bi)
			IRCD->SendNumeric(317, source.GetSource(), "%s %ld %ld :seconds idle, signon time", bi->nick.c_str(), static_cast<long>(Anope::CurTime - bi->lastmsg), static_cast<long>(bi->signon));
		IRCD->SendNumeric(318, source.GetSource(), "%s :End of /WHOIS list.", params[0].c_str());
	}
	else
		IRCD->SendNumeric(401, source.GetSource(), "%s :No such user.", params[0].c_str());

	return;
}

