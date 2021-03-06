/* NickServ core functions
 *
 * (C) 2003-2012 Anope Team
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 */

/*************************************************************************/

#include "module.h"

class CommandNSInfo : public Command
{
 private:
	template<typename T> void CheckOptStr(NickCore *core, Anope::string &buf, T opt, const char *str, const Flags<T> *nc, bool reverse_logic = false)
	{
		if (reverse_logic ? !nc->HasFlag(opt) : nc->HasFlag(opt))
		{
			if (!buf.empty())
				buf += ", ";

			buf += Language::Translate(core, str);
		}
	}
 public:
	CommandNSInfo(Module *creator) : Command(creator, "nickserv/info", 0, 2)
	{
		this->SetFlag(CFLAG_ALLOW_UNREGISTERED);
		this->SetDesc(_("Displays information about a given nickname"));
		this->SetSyntax(_("[\037nickname\037]"));
	}

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) anope_override
	{

		const Anope::string &nick = params.size() ? params[0] : (source.nc ? source.nc->display : source.GetNick());
		NickAlias *na = NickAlias::Find(nick);
		bool has_auspex = source.HasPriv("nickserv/auspex");

		if (!na)
		{
			if (BotInfo::Find(nick, true))
				source.Reply(_("Nick \002%s\002 is part of this Network's Services."), nick.c_str());
			else
				source.Reply(NICK_X_NOT_REGISTERED, nick.c_str());
		}
		else
		{
			bool nick_online = false, show_hidden = false;

			/* Is the real owner of the nick we're looking up online? -TheShadow */
			User *u2 = User::Find(na->nick);
			if (u2 && u2->Account() == na->nc)
				nick_online = true;

			if (has_auspex || na->nc == source.GetAccount())
				show_hidden = true;

			source.Reply(_("%s is %s"), na->nick.c_str(), na->last_realname.c_str());

			if (na->nc->HasFlag(NI_UNCONFIRMED))
				source.Reply(_("%s nickname is unconfirmed."), na->nick.c_str());

			if (na->nc->IsServicesOper() && (show_hidden || !na->nc->HasFlag(NI_HIDE_STATUS)))
				source.Reply(_("%s is a services operator of type %s."), na->nick.c_str(), na->nc->o->ot->GetName().c_str());

			InfoFormatter info(source.nc);

			if (nick_online)
			{
				if (show_hidden && !na->last_realhost.empty())
					info[_("Online from")] = na->last_realhost;
				if (show_hidden || !na->nc->HasFlag(NI_HIDE_MASK))
					info[_("Online from")] = na->last_usermask;
				else
					source.Reply(_("%s is currently online."), na->nick.c_str());
			}
			else
			{
				if (show_hidden || !na->nc->HasFlag(NI_HIDE_MASK))
					info[_("Last seen address")] = na->last_usermask;
				if (show_hidden && !na->last_realhost.empty())
					info[_("Last seen address")] = na->last_realhost;
			}

			info[_("Time registered")] = Anope::strftime(na->time_registered);

			if (!nick_online)
				info[_("Last seen")] = Anope::strftime(na->last_seen);

			if (!na->last_quit.empty() && (show_hidden || !na->nc->HasFlag(NI_HIDE_QUIT)))
				info[_("Last quit message")] = na->last_quit;

			if (!na->nc->email.empty() && (show_hidden || !na->nc->HasFlag(NI_HIDE_EMAIL)))
				info[_("Email address")] = na->nc->email;

			if (show_hidden)
			{
				if (na->HasVhost())
				{
					if (IRCD->CanSetVIdent && !na->GetVhostIdent().empty())
						info[_("VHost")] = na->GetVhostIdent() + "@" + na->GetVhostHost();
					else
						info[_("VHost")] = na->GetVhostHost();
				}

				if (!na->nc->greet.empty())
					info[_("Greet")] = na->nc->greet;

				Anope::string optbuf;

				CheckOptStr<NickCoreFlag>(source.nc, optbuf, NI_KILLPROTECT, _("Protection"), na->nc);
				CheckOptStr<NickCoreFlag>(source.nc, optbuf, NI_SECURE, _("Security"), na->nc);
				CheckOptStr<NickCoreFlag>(source.nc, optbuf, NI_PRIVATE, _("Private"), na->nc);
				CheckOptStr<NickCoreFlag>(source.nc, optbuf, NI_MSG, _("Message mode"), na->nc);
				CheckOptStr<NickCoreFlag>(source.nc, optbuf, NI_AUTOOP, _("Auto-op"), na->nc);
				CheckOptStr<NickCoreFlag>(source.nc, optbuf, NI_SUSPENDED, _("Suspended"), na->nc);
				CheckOptStr<NickCoreFlag>(source.nc, optbuf, NI_STATS, _("Chanstats"), na->nc);
				CheckOptStr<NickNameFlag>(source.nc, optbuf, NS_NO_EXPIRE, _("No expire"), na);

				info[_("Options")] = optbuf.empty() ? _("None") : optbuf;

				if (na->nc->HasFlag(NI_UNCONFIRMED) == false)
				{
					if (na->HasFlag(NS_NO_EXPIRE) || !Config->NSExpire)
						;
					else
						info[_("Expires")] = Anope::strftime(na->last_seen + Config->NSExpire);
				}
				else
					info[_("Expires")] = Anope::strftime(na->time_registered + Config->NSUnconfirmedExpire);
			}

			FOREACH_MOD(I_OnNickInfo, OnNickInfo(source, na, info, show_hidden));

			std::vector<Anope::string> replies;
			info.Process(replies);

			for (unsigned i = 0; i < replies.size(); ++i)
				source.Reply(replies[i]);
		}
	}

	bool OnHelp(CommandSource &source, const Anope::string &subcommand) anope_override
	{
		this->SendSyntax(source);
		source.Reply(" ");
		source.Reply(_("Displays information about the given nickname, such as\n"
				"the nick's owner, last seen address and time, and nick\n"
				"options. If no nick is given, and you are identified,\n"
				"your account name is used, else your current nickname is\n"
				"used."));

		return true;
	}
};

class NSInfo : public Module
{
	CommandNSInfo commandnsinfo;

 public:
	NSInfo(const Anope::string &modname, const Anope::string &creator) : Module(modname, creator, CORE),
		commandnsinfo(this)
	{
		this->SetAuthor("Anope");

	}
};

MODULE_INIT(NSInfo)
