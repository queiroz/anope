/* ChanServ core functions
 *
 * (C) 2003-2010 Anope Team
 * Contact us at team@anope.org
 *
 * Please read COPYING and README for further details.
 *
 * Based on the original code of Epona by Lara.
 * Based on the original code of Services by Andy Church.
 */

/*************************************************************************/

#include "module.h"

class CommandCSForbid : public Command
{
 public:
	CommandCSForbid() : Command("FORBID", 1, 2, "chanserv/forbid")
	{
		this->SetFlag(CFLAG_ALLOW_UNREGISTEREDCHANNEL);
	}

	CommandReturn Execute(User *u, const std::vector<ci::string> &params)
	{
		ChannelInfo *ci;
		const char *chan = params[0].c_str();
		const char *reason = params.size() > 1 ? params[1].c_str() : NULL;

		Channel *c;

		if (Config.ForceForbidReason && !reason)
		{
			syntax_error(Config.s_ChanServ, u, "FORBID", CHAN_FORBID_SYNTAX_REASON);
			return MOD_CONT;
		}

		if (*chan != '#')
		{
			notice_lang(Config.s_ChanServ, u, CHAN_SYMBOL_REQUIRED);
			return MOD_CONT;
		}

		if (readonly)
		{
			notice_lang(Config.s_ChanServ, u, READ_ONLY_MODE);
			return MOD_CONT;
		}

		if ((ci = cs_findchan(chan)))
			delete ci;

		ci = new ChannelInfo(chan);
		if (!ci)
		{
			Alog() << Config.s_ChanServ << ": Valid FORBID for " << ci->name << " by " << u->nick << " failed";
			notice_lang(Config.s_ChanServ, u, CHAN_FORBID_FAILED, chan);
			return MOD_CONT;
		}

		ci->SetFlag(CI_FORBIDDEN);
		ci->forbidby = sstrdup(u->nick.c_str());
		if (reason)
			ci->forbidreason = sstrdup(reason);

		if ((c = findchan(ci->name)))
		{
			/* Before banning everyone, it might be prudent to clear +e and +I lists..
			 * to prevent ppl from rejoining.. ~ Viper */
			c->ClearExcepts();
			c->ClearInvites();

			for (CUserList::iterator it = c->users.begin(), it_end = c->users.end(); it != it_end; )
			{
				UserContainer *uc = *it++;

				if (is_oper(uc->user))
					continue;

				c->Kick(ChanServ, uc->user, "%s", reason ? reason : getstring(uc->user->Account(), CHAN_FORBID_REASON));
			}
		}

		if (Config.WallForbid)
			ircdproto->SendGlobops(ChanServ, "\2%s\2 used FORBID on channel \2%s\2", u->nick.c_str(), ci->name.c_str());

		if (ircd->chansqline)
		{
			XLine x(chan, "Forbidden");
			ircdproto->SendSQLine(&x);
		}

		Alog() << Config.s_ChanServ << ": " << u->nick << " set FORBID for channel " << ci->name;
		notice_lang(Config.s_ChanServ, u, CHAN_FORBID_SUCCEEDED, chan);

		FOREACH_MOD(I_OnChanForbidden, OnChanForbidden(ci));

		return MOD_CONT;
	}

	bool OnHelp(User *u, const ci::string &subcommand)
	{
		notice_help(Config.s_ChanServ, u, CHAN_SERVADMIN_HELP_FORBID);
		return true;
	}

	void OnSyntaxError(User *u, const ci::string &subcommand)
	{
		syntax_error(Config.s_ChanServ, u, "FORBID", CHAN_FORBID_SYNTAX);
	}

	void OnServHelp(User *u)
	{
		notice_lang(Config.s_ChanServ, u, CHAN_HELP_CMD_FORBID);
	}
};

class CSForbid : public Module
{
 public:
	CSForbid(const std::string &modname, const std::string &creator) : Module(modname, creator)
	{
		this->SetAuthor("Anope");
		this->SetType(CORE);
		this->AddCommand(ChanServ, new CommandCSForbid());
	}
};

MODULE_INIT(CSForbid)