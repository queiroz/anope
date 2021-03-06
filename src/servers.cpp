/* Routines to maintain a list of connected servers
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
#include "xline.h"
#include "servers.h"
#include "bots.h"
#include "regchannel.h"
#include "protocol.h"
#include "config.h"
#include "channels.h"

const Anope::string ServerFlagStrings[] = { "SERVER_NONE", "SERVER_SYNCING", "SERVER_JUPED", "" };
template<> const Anope::string* Flags<ServerFlag>::flags_strings = ServerFlagStrings;

/* Anope */
Server *Me = NULL;

std::set<Anope::string> Servers::Capab;

Server::Server(Server *up, const Anope::string &sname, unsigned shops, const Anope::string &desc, const Anope::string &ssid, ServerFlag flag) : name(sname), hops(shops), description(desc), sid(ssid), uplink(up)
{
	this->SetFlag(SERVER_SYNCING);
	this->SetFlag(flag);

	Log(this, "connect") << "uplinked to " << (this->uplink ? this->uplink->GetName() : "no uplink") << " connected to the network";

	/* Add this server to our uplinks leaf list */
	if (this->uplink)
	{
		this->uplink->AddLink(this);

		/* Check to be sure this isn't a juped server */
		if (Me == this->uplink && !this->HasFlag(SERVER_JUPED))
		{
			/* Now do mode related stuff as we know what modes exist .. */
			for (botinfo_map::iterator it = BotListByNick->begin(), it_end = BotListByNick->end(); it != it_end; ++it)
			{
				BotInfo *bi = it->second;
				Anope::string modes = !bi->botmodes.empty() ? ("+" + bi->botmodes) : IRCD->DefaultPseudoclientModes;

				bi->SetModesInternal(modes.c_str());
				for (unsigned i = 0; i < bi->botchannels.size(); ++i)
				{
					size_t h = bi->botchannels[i].find('#');
					if (h == Anope::string::npos)
						continue;
					Anope::string chname = bi->botchannels[i].substr(h);
					Channel *c = Channel::Find(chname);
					if (c && c->FindUser(bi))
					{
						Anope::string want_modes = bi->botchannels[i].substr(0, h);
						for (unsigned j = 0; j < want_modes.length(); ++j)
						{
							ChannelMode *cm = ModeManager::FindChannelModeByChar(want_modes[j]);
							if (cm == NULL)
								cm = ModeManager::FindChannelModeByChar(ModeManager::GetStatusChar(want_modes[j]));
							if (cm && cm->type == MODE_STATUS)
							{
								MessageSource ms = bi;
								c->SetModeInternal(ms, cm, bi->nick);
							}
						}
					}
				}
			}

			IRCD->SendBOB();
	
			for (unsigned i = 0; i < Me->GetLinks().size(); ++i)
			{
				Server *s = Me->GetLinks()[i];

				if (s->HasFlag(SERVER_JUPED))
					IRCD->SendServer(s);
			}

			/* We make the bots go online */
			for (user_map::const_iterator it = UserListByNick.begin(); it != UserListByNick.end(); ++it)
			{
				User *u = it->second;

				BotInfo *bi = BotInfo::Find(u->nick);
				if (bi)
				{
					XLine x(bi->nick, "Reserved for services");
					IRCD->SendSQLine(NULL, &x);
				}

				IRCD->SendClientIntroduction(u);
				if (bi)
					bi->introduced = true;
			}

			for (channel_map::const_iterator it = ChannelList.begin(), it_end = ChannelList.end(); it != it_end; ++it)
			{
				Channel *c = it->second;
				if (c->users.empty())
					IRCD->SendChannel(c);
				else
					for (CUserList::const_iterator cit = c->users.begin(), cit_end = c->users.end(); cit != cit_end; ++cit)
						IRCD->SendJoin((*cit)->user, c, (*cit)->status);
			}
		}
	}

	FOREACH_MOD(I_OnNewServer, OnNewServer(this));
}

Server::~Server()
{
	Log(this, "quit") << "quit from " << (this->uplink ? this->uplink->GetName() : "no uplink") << " for " << this->quit_reason;

	if (Servers::Capab.count("NOQUIT") > 0 || Servers::Capab.count("QS") > 0)
	{
		for (user_map::const_iterator it = UserListByNick.begin(); it != UserListByNick.end();)
		{
			User *u = it->second;
			++it;

			if (u->server == this)
			{
				NickAlias *na = NickAlias::Find(u->nick);
				if (na && !na->nc->HasFlag(NI_SUSPENDED) && (u->IsRecognized() || u->IsIdentified()))
				{
					na->last_seen = Anope::CurTime;
					na->last_quit = this->quit_reason;
				}

				delete u;
			}
		}

		Log(LOG_DEBUG) << "Finished removing all users for " << this->GetName();
	}

	if (this->uplink)
		this->uplink->DelLink(this);

	for (unsigned i = this->links.size(); i > 0; --i)
		this->links[i - 1]->Delete(this->quit_reason);
}

void Server::Delete(const Anope::string &reason)
{
	this->quit_reason = reason;
	FOREACH_MOD(I_OnServerQuit, OnServerQuit(this));
	delete this;
}

const Anope::string &Server::GetName() const
{
	return this->name;
}

unsigned Server::GetHops() const
{
	return this->hops;
}

void Server::SetDescription(const Anope::string &desc)
{
	this->description = desc;
}

const Anope::string &Server::GetDescription() const
{
	return this->description;
}

void Server::SetSID(const Anope::string &nsid)
{
	this->sid = nsid;
}

const Anope::string &Server::GetSID() const
{
	if (!this->sid.empty() && IRCD->RequiresID)
		return this->sid;
	else
		return this->name;
}

const std::vector<Server *> &Server::GetLinks() const
{
	return this->links;
}

Server *Server::GetUplink()
{
	return this->uplink;
}

void Server::AddLink(Server *s)
{
	this->links.push_back(s);

	Log(this, "connect") << "introduced " << s->GetName();
}

void Server::DelLink(Server *s)
{
	if (this->links.empty())
		throw CoreException("Server::DelLink called on " + this->GetName() + " for " + s->GetName() + " but we have no links?");

	for (unsigned i = 0, j = this->links.size(); i < j; ++i)
	{
		if (this->links[i] == s)
		{
			this->links.erase(this->links.begin() + i);
			break;
		}
	}

	Log(this, "quit") << "quit " << s->GetName();
}

void Server::Sync(bool sync_links)
{
	if (this->IsSynced())
		return;

	this->UnsetFlag(SERVER_SYNCING);

	Log(this, "sync") << "is done syncing";

	FOREACH_MOD(I_OnServerSync, OnServerSync(this));

	if (sync_links && !this->links.empty())
	{
		for (unsigned i = 0, j = this->links.size(); i < j; ++i)
			this->links[i]->Sync(true);
	}

	if (this->GetUplink() && this->GetUplink() == Me)
	{
		for (registered_channel_map::iterator it = RegisteredChannelList->begin(), it_end = RegisteredChannelList->end(); it != it_end; ++it)
		{
			ChannelInfo *ci = it->second;
			if (ci->HasFlag(CI_PERSIST))
			{
				bool created = false;
				if (!ci->c)
				{
					ci->c = new Channel(ci->name, ci->time_registered);
					created = true;
				}
				if (ModeManager::FindChannelModeByName(CMODE_PERM) != NULL)
				{
					ci->c->SetMode(NULL, CMODE_PERM);
					if (created)
						IRCD->SendChannel(ci->c);
				}
				else
				{
					if (!ci->bi)
						ci->WhoSends()->Assign(NULL, ci);
					if (ci->c->FindUser(ci->bi) == NULL)
						ci->bi->Join(ci->c);
				}
			}
		}

		FOREACH_MOD(I_OnPreUplinkSync, OnPreUplinkSync(this));

		IRCD->SendEOB();
		Me->Sync(false);

		FOREACH_MOD(I_OnUplinkSync, OnUplinkSync(this));

		for (channel_map::const_iterator it = ChannelList.begin(), it_end = ChannelList.end(); it != it_end; ++it)
		{
			Channel *c = it->second;
			c->CheckModes();
			if (c->ci)
				c->ci->RestoreTopic();
		}

		if (!Anope::NoFork && Anope::AtTerm())
		{
			Log(LOG_TERMINAL) << "Successfully linked, launching into background...";
			Anope::Fork();
		}
	}
}

bool Server::IsSynced() const
{
	return !this->HasFlag(SERVER_SYNCING);
}

bool Server::IsULined() const
{
	if (this == Me)
		return true;

	for (std::list<Anope::string>::const_iterator it = Config->Ulines.begin(), it_end = Config->Ulines.end(); it != it_end; ++it)
		if (it->equals_ci(this->GetName()))
			return true;
	return false;
}

void Server::Notice(const BotInfo *source, const Anope::string &message)
{
	if (Config->NSDefFlags.HasFlag(NI_MSG))
		IRCD->SendGlobalPrivmsg(source, this, message);
	else
		IRCD->SendGlobalNotice(source, this, message);
}

Server *Server::Find(const Anope::string &name, Server *s)
{
	Log(LOG_DEBUG_2) << "Server::Find called for " << name;

	if (!s)
		s = Me;
	if (s->GetName().equals_ci(name) || s->GetSID().equals_cs(name))
		return s;

	if (!s->GetLinks().empty())
	{
		for (unsigned i = 0, j = s->GetLinks().size(); i < j; ++i)
		{
			Server *serv = s->GetLinks()[i];

			if (serv->GetName().equals_ci(name) || serv->GetSID().equals_cs(name))
				return serv;
			Log(LOG_DEBUG_2) << "Server::Find checking " << serv->GetName() << " server tree for " << name;
			Server *server = Server::Find(name, serv);
			if (server)
				return server;
		}
	}

	return NULL;
}

/*************************************************************************/

static inline char& nextID(char &c)
{
	if (c == 'Z')
		c = '0';
	else if (c != '9')
		++c;
	else
		c = 'A';
	return c;
}

const Anope::string Servers::TS6_UID_Retrieve()
{
	if (!IRCD || !IRCD->RequiresID)
		return "";

	static Anope::string current_uid = "AAAAAA";

	while (User::Find(Config->Numeric + current_uid) != NULL)
	{
		int current_len = current_uid.length() - 1;
		while (current_len >= 0 && nextID(current_uid[current_len--]) == 'A');
	}

	return Config->Numeric + current_uid;
}

const Anope::string Servers::TS6_SID_Retrieve()
{
	if (!IRCD || !IRCD->RequiresID)
		return "";

	static Anope::string current_sid;
	if (current_sid.empty())
	{
		current_sid = Config->Numeric;
		if (current_sid.empty())
			current_sid = "00A";
	}

	while (Server::Find(current_sid) != NULL)
	{
		int current_len = current_sid.length() - 1;
		while (current_len >= 0 && nextID(current_sid[current_len--]) == 'A');
	}

	return current_sid;
}

Server* Servers::GetUplink()
{
	for (unsigned i = 0; Me && i < Me->GetLinks().size(); ++i)
		if (!Me->GetLinks()[i]->HasFlag(SERVER_JUPED))
			return Me->GetLinks()[i];
	return NULL;
}

