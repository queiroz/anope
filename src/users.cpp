/* Routines to maintain a list of online users.
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
#include "account.h"
#include "protocol.h"
#include "servers.h"
#include "channels.h"
#include "bots.h"
#include "config.h"
#include "opertype.h"
#include "nickserv.h"
#include "language.h"

user_map UserListByNick, UserListByUID;

int OperCount = 0;
unsigned MaxUserCount = 0;
time_t MaxUserTime = 0;

User::User(const Anope::string &snick, const Anope::string &sident, const Anope::string &shost, const Anope::string &svhost, const Anope::string &sip, Server *sserver, const Anope::string &srealname, time_t ssignon, const Anope::string &smodes, const Anope::string &suid)
{
	if (snick.empty() || sident.empty() || shost.empty())
		throw CoreException("Bad args passed to User::User");

	/* we used to do this by calloc, no more. */
	server = NULL;
	invalid_pw_count = invalid_pw_time = lastmemosend = lastnickreg = lastmail = 0;
	on_access = false;

	this->nick = snick;
	this->ident = sident;
	this->host = shost;
	this->vhost = svhost;
	if (!svhost.empty())
		this->SetCloakedHost(svhost);
	this->ip = sip;
	this->server = sserver;
	this->realname = srealname;
	this->timestamp = this->signon = ssignon;
	this->SetModesInternal("%s", smodes.c_str());
	this->uid = suid;
	this->super_admin = false;

	size_t old = UserListByNick.size();
	UserListByNick[snick] = this;
	if (!suid.empty())
		UserListByUID[suid] = this;
	if (old == UserListByNick.size())
		Log(LOG_DEBUG) << "Duplicate user " << snick << " in user table?";

	this->nc = NULL;

	if (sserver) // Our bots are introduced on startup with no server
	{
		++sserver->users;
		Log(this, "connect") << (!svhost.empty() ? Anope::string("(") + svhost + ") " : "") << "(" << srealname << ") " << sip << " connected to the network (" << sserver->GetName() << ")";
	}

	if (UserListByNick.size() > MaxUserCount)
	{
		MaxUserCount = UserListByNick.size();
		MaxUserTime = Anope::CurTime;
		Log(this, "maxusers") << "connected - new maximum user count: " << UserListByNick.size();
	}

	bool exempt = false;
	if (server && server->IsULined())
		exempt = true;
	Reference<User> user = this;
	FOREACH_MOD(I_OnUserConnect, OnUserConnect(user, exempt));
}

void User::ChangeNick(const Anope::string &newnick, time_t ts)
{
	/* Sanity check to make sure we don't segfault */
	if (newnick.empty())
		throw CoreException("User::ChangeNick() got a bad argument");
	
	this->super_admin = false;
	Log(this, "nick") << "(" << this->realname << ") changed nick to " << newnick;

	Anope::string old = this->nick;
	this->timestamp = ts;

	if (this->nick.equals_ci(newnick))
		this->nick = newnick;
	else
	{
		NickAlias *old_na = NickAlias::Find(this->nick);
		if (old_na && (this->IsIdentified(true) || this->IsRecognized()))
			old_na->last_seen = Anope::CurTime;
		
		UserListByNick.erase(this->nick);
		this->nick = newnick;
		UserListByNick[this->nick] = this;

		on_access = false;
		NickAlias *na = NickAlias::Find(this->nick);
		if (na)
			on_access = na->nc->IsOnAccess(this);

		if (old_na)
			old_na->OnCancel(this);

		if (na && na->nc == this->Account())
		{
			na->last_seen = Anope::CurTime;
			this->UpdateHost();
		}
	}

	FOREACH_MOD(I_OnUserNickChange, OnUserNickChange(this, old));
}

void User::SetDisplayedHost(const Anope::string &shost)
{
	if (shost.empty())
		throw CoreException("empty host? in MY services? it seems it's more likely than I thought.");

	this->vhost = shost;

	Log(this, "host") << "changed vhost to " << shost;

	this->UpdateHost();
}

const Anope::string &User::GetDisplayedHost() const
{
	if (!this->vhost.empty())
		return this->vhost;
	else if (this->HasMode(UMODE_CLOAK) && !this->GetCloakedHost().empty())
		return this->GetCloakedHost();
	else
		return this->host;
}

void User::SetCloakedHost(const Anope::string &newhost)
{
	if (newhost.empty())
		throw "empty host in User::SetCloakedHost";

	chost = newhost;

	Log(this, "host") << "changed cloaked host to " << newhost;

	this->UpdateHost();
}

const Anope::string &User::GetCloakedHost() const
{
	return chost;
}

const Anope::string &User::GetUID() const
{
	if (!this->uid.empty() && IRCD->RequiresID)
		return this->uid;
	else
		return this->nick;
}

void User::SetVIdent(const Anope::string &sident)
{
	this->vident = sident;

	Log(this, "ident") << "changed vident to " << sident;

	this->UpdateHost();
}

const Anope::string &User::GetVIdent() const
{
	if (!this->vident.empty())
		return this->vident;
	else
		return this->ident;
}

void User::SetIdent(const Anope::string &sident)
{
	this->ident = sident;

	Log(this, "ident") << "changed real ident to " << sident;

	this->UpdateHost();
}

const Anope::string &User::GetIdent() const
{
	return this->ident;
}

Anope::string User::GetMask() const
{
	return this->nick + "!" + this->ident + "@" + this->host;
}

Anope::string User::GetDisplayedMask() const
{
	return this->nick + "!" + this->GetVIdent() + "@" + this->GetDisplayedHost();
}

void User::SetRealname(const Anope::string &srealname)
{
	if (srealname.empty())
		throw CoreException("realname empty in SetRealname");

	this->realname = srealname;
	NickAlias *na = NickAlias::Find(this->nick);

	if (na && (this->IsIdentified(true) || this->IsRecognized()))
		na->last_realname = srealname;

	Log(this, "realname") << "changed realname to " << srealname;
}

User::~User()
{
	Log(LOG_DEBUG_2) << "User::~User() called";

	Log(this, "disconnect") << "(" << this->realname << ") " << "disconnected from the network (" << this->server->GetName() << ")";
	--this->server->users;

	FOREACH_MOD(I_OnUserLogoff, OnUserLogoff(this));

	ModeManager::StackerDel(this);
	this->Logout();

	if (this->HasMode(UMODE_OPER))
		--OperCount;

	while (!this->chans.empty())
		this->chans.front()->chan->DeleteUser(this);

	UserListByNick.erase(this->nick);
	if (!this->uid.empty())
		UserListByUID.erase(this->uid);

	NickAlias *na = NickAlias::Find(this->nick);
	if (na)
		na->OnCancel(this);

	Log(LOG_DEBUG_2) << "User::~User() done";
}

void User::SendMessage(const BotInfo *source, const char *fmt, ...)
{
	va_list args;
	char buf[BUFSIZE] = "";

	const char *translated_message = Language::Translate(this, fmt);

	va_start(args, fmt);
	vsnprintf(buf, BUFSIZE - 1, translated_message, args);

	this->SendMessage(source, Anope::string(buf));

	va_end(args);
}

void User::SendMessage(const BotInfo *source, const Anope::string &msg)
{
	const char *translated_message = Language::Translate(this, msg.c_str());

	/* Send privmsg instead of notice if:
	* - UsePrivmsg is enabled
	* - The user is not registered and NSDefMsg is enabled
	* - The user is registered and has set /ns set msg on
	*/
	sepstream sep(translated_message, '\n');
	Anope::string tok;
	while (sep.GetToken(tok))
	{
		if (Config->UsePrivmsg && ((!this->nc && Config->NSDefFlags.HasFlag(NI_MSG)) || (this->nc && this->nc->HasFlag(NI_MSG))))
			IRCD->SendPrivmsg(source, this->GetUID(), "%s", tok.c_str());
		else
			IRCD->SendNotice(source, this->GetUID(), "%s", tok.c_str());
	}
}

/** Collides a nick.
 *
 * First, it marks the nick (if the user is on a registered nick, we don't use it without but it could be)
 * as COLLIDED, this is checked in NickAlias::OnCancel.
 *
 * Then it does one of two things.
 *
 * 1. This will force change the users nick to the guest nick. This gets processed by the IRCd and comes
 *    back to call do_nick. do_nick changes the nick of the use to the new one, then calls NickAlias::OnCancel
 *    with the users old nick's nickalias (if there is one).
 *
 * 2. Calls User::Kill, which will either delete the user immediatly or kill them, wait for the QUIT,
 *    then delete the user then. Users destructor then calls NickAlias::OnCancel
 *
 * NickAlias::OnCancel checks for NS_COLLIDED, it then does one of two things.
 *
 * 1. If supported, we send a SVSHold for the user. We are done here, the IRCds expires this at the time we give it.
 *
 * 2. We create a new client with SendClientIntroduction(). Note that is it important that this is called either after the
 *    user has been removed from our internal list of user or after the users nick has been updated completely internally.
 *    This is beacuse SendClientIntroduction will destroy any users we think are currently on the nickname (which causes a
 *    lot of problems, eg, deleting the user which recalls OnCancel), whether they really are or not. We then create a
 *    release timer for this new client that waits and later on sends a QUIT for the client. Release timers are never used
 *    for SVSHolds. Ever.
 *
 *
 *  Note that now for the timers we only store the users name, not the NickAlias* pointer. We never remove timers when
 *  a user changes nick or a nick is deleted, the timers must assume that either of these may have happend.
 *
 *  Storing NickAlias* pointers caused quite a problem, some of which are:
 *
 *  Having a valid timer alive that calls User::Collide would either:
 *
 *  1. Kill the user, causing users destructor to cancel all timers for the nick (as it should, it has no way of knowing
 *     if we are in a timer or not) which would delete the currently active timer while it was running, causing TimerManager
 *     to explode.
 *
 *  2. Force a user off of their nick, this would call NickAlias::Cancel before updating the user internally (to cancel the
 *     current nicks timers, granted we could have easially saved this and called it after) which could possibly try to
 *     introduce an enforcer nick. We would then check to see if the nick is already in use (it is, internally) and send
 *     a kill for that nick. That may in turn delete the user immediatly, calling users destructor, which would attempt to
 *     delete the timer, causing TimerManager to explode.
 *
 *     Additionally, if we marked the timer as "in use" so that calling the ClearTimer function wouldn't delete them, users
 *     destructor would then call NickAlias::OnCancel, which would (at this point, it was unsetting GUESTED after introducing
 *     the new client) introduce the same new client again, without actually deleting the originial user, causing an infinite
 *     loop.
 *
 *     This is why we remove NS_GUESTED first in NickAlias::OnCancel before introducing a new client, although this should
 *     not happen anymore. If I must emphasize this again, users need to be GONE from the internal list before calling
 *     NickAlias::OnCancel. NickAlias::OnCancel intentionally reffers to this->nick, not the user passed to it. They *can*
 *     (but not always) be different, depending if the user changed nicks or disconnected.
 *
 *
 *  Adam
 */
void User::Collide(NickAlias *na)
{
	if (na)
		na->SetFlag(NS_COLLIDED);

	if (IRCD->CanSVSNick)
	{
		Anope::string guestnick;

		int i = 0;
		do
		{
			guestnick = Config->NSGuestNickPrefix + stringify(static_cast<uint16_t>(rand()));
		} while (User::Find(guestnick) && i++ < 10);

		if (i == 11)
			this->Kill(Config->NickServ, "Services nickname-enforcer kill");
		else
		{
			if (NickServ)
				this->SendMessage(NickServ, _("Your nickname is now being changed to \002%s\002"), guestnick.c_str());
			IRCD->SendForceNickChange(this, guestnick, Anope::CurTime);
		}
	}
	else
		this->Kill(Config->NickServ, "Services nickname-enforcer kill");
}

void User::Identify(NickAlias *na)
{
	if (!na)
	{
		Log() << "User::Identify() called with NULL pointer";
		return;
	}

	if (this->nick.equals_ci(na->nick))
	{
		Anope::string last_usermask = this->GetIdent() + "@" + this->GetDisplayedHost();
		Anope::string last_realhost = this->GetIdent() + "@" + this->host;
		na->last_usermask = last_usermask;
		na->last_realhost = last_realhost;
		na->last_realname = this->realname;
		na->last_seen = Anope::CurTime;
	}

	this->Login(na->nc);
	IRCD->SendLogin(this);

	const NickAlias *this_na = NickAlias::Find(this->nick);
	if (!Config->NoNicknameOwnership && this_na && this_na->nc == *na->nc && na->nc->HasFlag(NI_UNCONFIRMED) == false)
		this->SetMode(NickServ, UMODE_REGISTERED);

	FOREACH_MOD(I_OnNickIdentify, OnNickIdentify(this));

	if (this->IsServicesOper())
	{
		if (!this->nc->o->ot->modes.empty())
		{
			this->SetModes(OperServ, "%s", this->nc->o->ot->modes.c_str());
			if (OperServ)
				this->SendMessage(OperServ, "Changing your usermodes to \002%s\002", this->nc->o->ot->modes.c_str());
			UserMode *um = ModeManager::FindUserModeByName(UMODE_OPER);
			if (um && !this->HasMode(UMODE_OPER) && this->nc->o->ot->modes.find(um->mchar) != Anope::string::npos)
				IRCD->SendOper(this);
		}
		if (IRCD->CanSetVHost && !this->nc->o->vhost.empty())
		{
			if (OperServ)
				this->SendMessage(OperServ, "Changing your vhost to \002%s\002", this->nc->o->vhost.c_str());
 			this->SetDisplayedHost(this->nc->o->vhost);
			IRCD->SendVhost(this, "", this->nc->o->vhost);
		}
	}
}


void User::Login(NickCore *core)
{
	this->Logout();
	this->nc = core;
	core->users.push_back(this);

	this->UpdateHost();

	if (this->server->IsSynced())
		Log(this, "account") << "is now identified as " << this->nc->display;
}

void User::Logout()
{
	if (!this->nc)
		return;
	
	Log(this, "account") << "is no longer identified as " << this->nc->display;

	std::list<User *>::iterator it = std::find(this->nc->users.begin(), this->nc->users.end(), this);
	if (it != this->nc->users.end())
		this->nc->users.erase(it);

	this->nc = NULL;
}

NickCore *User::Account() const
{
	return this->nc;
}

bool User::IsIdentified(bool CheckNick) const
{
	if (CheckNick && this->nc)
	{
		NickAlias *na = NickAlias::Find(this->nc->display);

		if (na && *na->nc == *this->nc)
			return true;

		return false;
	}

	return this->nc ? true : false;
}

bool User::IsRecognized(bool CheckSecure) const
{
	if (CheckSecure && on_access)
	{
		const NickAlias *na = NickAlias::Find(this->nick);

		if (!na || na->nc->HasFlag(NI_SECURE))
			return false;
	}

	return on_access;
}

bool User::IsServicesOper()
{
	if (!this->nc || !this->nc->IsServicesOper())
		// No opertype.
		return false;
	else if (this->nc->o->require_oper && !this->HasMode(UMODE_OPER))
		return false;
	else if (!this->nc->o->certfp.empty() && this->fingerprint != this->nc->o->certfp)
		// Certfp mismatch
		return false;
	else if (!this->nc->o->hosts.empty())
	{
		bool match = false;
		Anope::string match_host = this->GetIdent() + "@" + this->host;
		for (unsigned i = 0; i < this->nc->o->hosts.size(); ++i)
			if (Anope::Match(match_host, this->nc->o->hosts[i]))
				match = true;
		if (match == false)
			return false;
	}

	EventReturn MOD_RESULT;
	FOREACH_RESULT(I_IsServicesOper, IsServicesOper(this));
	if (MOD_RESULT == EVENT_STOP)
		return false;

	return true;
}

bool User::HasCommand(const Anope::string &command)
{
	if (this->IsServicesOper())
		return this->nc->o->ot->HasCommand(command);
	return false;
}

bool User::HasPriv(const Anope::string &priv)
{
	if (this->IsServicesOper())
		return this->nc->o->ot->HasPriv(priv);
	return false;
}

void User::UpdateHost()
{
	if (this->host.empty())
		return;

	NickAlias *na = NickAlias::Find(this->nick);
	on_access = false;
	if (na)
		on_access = na->nc->IsOnAccess(this);

	if (na && (this->IsIdentified(true) || this->IsRecognized()))
	{
		Anope::string last_usermask = this->GetIdent() + "@" + this->GetDisplayedHost();
		Anope::string last_realhost = this->GetIdent() + "@" + this->host;
		na->last_usermask = last_usermask;
		na->last_realhost = last_realhost;
	}
}

bool User::HasMode(UserModeName Name) const
{
	return this->modes.HasFlag(Name);
}

void User::SetModeInternal(UserMode *um, const Anope::string &param)
{
	if (!um)
		return;

	this->modes.SetFlag(um->name);
	if (!param.empty())
		this->mode_params.insert(std::make_pair(um->name, param));

	FOREACH_MOD(I_OnUserModeSet, OnUserModeSet(this, um->name));
}

void User::RemoveModeInternal(UserMode *um)
{
	if (!um)
		return;

	this->modes.UnsetFlag(um->name);
	std::map<UserModeName, Anope::string>::iterator it = this->mode_params.find(um->name);
	if (it != this->mode_params.end())
		this->mode_params.erase(it);

	FOREACH_MOD(I_OnUserModeUnset, OnUserModeUnset(this, um->name));
}

void User::SetMode(const BotInfo *bi, UserMode *um, const Anope::string &Param)
{
	if (!um || HasMode(um->name))
		return;

	ModeManager::StackerAdd(bi, this, um, true, Param);
	SetModeInternal(um, Param);
}

void User::SetMode(const BotInfo *bi, UserModeName Name, const Anope::string &Param)
{
	SetMode(bi, ModeManager::FindUserModeByName(Name), Param);
}

void User::RemoveMode(const BotInfo *bi, UserMode *um)
{
	if (!um || !HasMode(um->name))
		return;

	ModeManager::StackerAdd(bi, this, um, false);
	RemoveModeInternal(um);
}

void User::RemoveMode(const BotInfo *bi, UserModeName Name)
{
	RemoveMode(bi, ModeManager::FindUserModeByName(Name));
}

void User::SetModes(const BotInfo *bi, const char *umodes, ...)
{
	char buf[BUFSIZE] = "";
	va_list args;
	Anope::string modebuf, sbuf;
	int add = -1;
	va_start(args, umodes);
	vsnprintf(buf, BUFSIZE - 1, umodes, args);
	va_end(args);

	spacesepstream sep(buf);
	sep.GetToken(modebuf);
	for (unsigned i = 0, end = modebuf.length(); i < end; ++i)
	{
		UserMode *um;

		switch (modebuf[i])
		{
			case '+':
				add = 1;
				continue;
			case '-':
				add = 0;
				continue;
			default:
				if (add == -1)
					continue;
				um = ModeManager::FindUserModeByChar(modebuf[i]);
				if (!um)
					continue;
		}

		if (add)
		{
			if (um->type == MODE_PARAM && sep.GetToken(sbuf))
				this->SetMode(bi, um, sbuf);
			else
				this->SetMode(bi, um);
		}
		else
			this->RemoveMode(bi, um);
	}
}

void User::SetModesInternal(const char *umodes, ...)
{
	char buf[BUFSIZE] = "";
	va_list args;
	Anope::string modebuf, sbuf;
	int add = -1;
	va_start(args, umodes);
	vsnprintf(buf, BUFSIZE - 1, umodes, args);
	va_end(args);

	Log(this, "mode") << "changes modes to " << buf;

	spacesepstream sep(buf);
	sep.GetToken(modebuf);
	for (unsigned i = 0, end = modebuf.length(); i < end; ++i)
	{
		UserMode *um;

		switch (modebuf[i])
		{
			case '+':
				add = 1;
				continue;
			case '-':
				add = 0;
				continue;
			default:
				if (add == -1)
					continue;
				um = ModeManager::FindUserModeByChar(modebuf[i]);
				if (!um)
					continue;
		}

		if (add)
		{
			if (um->type == MODE_PARAM && sep.GetToken(sbuf))
				this->SetModeInternal(um, sbuf);
			else
				this->SetModeInternal(um);
		}
		else
			this->RemoveModeInternal(um);

		switch (um->name)
		{
			case UMODE_OPER:
				if (add)
					++OperCount;
				else
					--OperCount;
				break;
			case UMODE_CLOAK:
			case UMODE_VHOST:
				if (!add && !this->vhost.empty())
					this->vhost.clear();
				this->UpdateHost();
			default:
				break;
		}
	}
}

Anope::string User::GetModes() const
{
	Anope::string ret;

	for (size_t i = UMODE_BEGIN + 1; i < UMODE_END; ++i)
		if (this->modes.HasFlag(static_cast<UserModeName>(i)))
		{
			UserMode *um = ModeManager::FindUserModeByName(static_cast<UserModeName>(i));
			if (um == NULL)
				continue;
			ret += um->mchar;
		}

	return ret;
}

ChannelContainer *User::FindChannel(const Channel *c) const
{
	for (UChannelList::const_iterator it = this->chans.begin(), it_end = this->chans.end(); it != it_end; ++it)
		if ((*it)->chan == c)
			return *it;
	return NULL;
}

bool User::IsProtected() const
{
	if (this->HasMode(UMODE_PROTECTED) || this->HasMode(UMODE_GOD))
		return true;

	return false;
}

void User::Kill(const Anope::string &source, const Anope::string &reason)
{
	Anope::string real_source = source.empty() ? Config->ServerName : source;
	Anope::string real_reason = real_source + " (" + reason + ")";

	IRCD->SendSVSKill(BotInfo::Find(source), this, "%s", real_reason.c_str());
}

void User::KillInternal(const Anope::string &source, const Anope::string &reason)
{
	Log(this, "killed") << "was killed by " << source << " (Reason: " << reason << ")";

	NickAlias *na = NickAlias::Find(this->nick);
	if (na && !na->nc->HasFlag(NI_SUSPENDED) && (this->IsRecognized() || this->IsIdentified(true)))
	{
		na->last_seen = Anope::CurTime;
		na->last_quit = reason;
	}

	delete this;
}

Anope::string User::Mask() const
{
	Anope::string mask;
	Anope::string mident = this->GetIdent();
	Anope::string mhost = this->GetDisplayedHost();

	if (mident[0] == '~')
		mask = "*" + mident + "@";
	else
		mask = mident + "@";

	size_t dot;
	/* To make sure this is an IP, make sure the host contains only numbers and dots, and check to make sure it only contains 3 dots */
	if (mhost.find_first_not_of("0123456789.") == Anope::string::npos && (dot = mhost.find('.')) != Anope::string::npos && (dot = mhost.find('.', dot + 1)) != Anope::string::npos && (dot = mhost.find('.', dot + 1)) != Anope::string::npos && mhost.find('.', dot + 1) == Anope::string::npos)
	{ /* IP addr */
		dot = mhost.find('.');
		mask += mhost.substr(0, dot) + ".*";
	}
	else
	{
		if ((dot = mhost.find('.')) != Anope::string::npos && mhost.find('.', dot + 1) != Anope::string::npos)
			mask += "*" + mhost.substr(dot);
		else
			mask += mhost;
	}

	return mask;
}

bool User::BadPassword()
{
	if (!Config->BadPassLimit)
		return false;

	if (Config->BadPassTimeout > 0 && this->invalid_pw_time > 0 && this->invalid_pw_time < Anope::CurTime - Config->BadPassTimeout)
		this->invalid_pw_count = 0;
	++this->invalid_pw_count;
	this->invalid_pw_time = Anope::CurTime;
	if (this->invalid_pw_count >= Config->BadPassLimit)
	{
		this->Kill(Config->ServerName, "Too many invalid passwords");
		return true;
	}

	return false;
}

User* User::Find(const Anope::string &name, bool nick_only)
{
	if (!nick_only && isdigit(name[0]) && IRCD->RequiresID)
	{
		user_map::iterator it = UserListByUID.find(name);
		if (it != UserListByUID.end())
			return it->second;
	}
	else
	{
		user_map::iterator it = UserListByNick.find(name);
		if (it != UserListByNick.end())
			return it->second;
	}

	return NULL;
}

