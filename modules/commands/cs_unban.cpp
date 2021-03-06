/* ChanServ core functions
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

class CommandCSUnban : public Command
{
 public:
	CommandCSUnban(Module *creator) : Command(creator, "chanserv/unban", 1, 2)
	{
		this->SetDesc(_("Remove all bans preventing a user from entering a channel"));
		this->SetSyntax(_("\037channel\037 [\037nick\037]"));
	}

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) anope_override
	{
		ChannelInfo *ci = ChannelInfo::Find(params[0]);
		if (ci == NULL)
		{
			source.Reply(CHAN_X_NOT_REGISTERED, params[0].c_str());
			return;
		}

		if (ci->c == NULL)
		{
			source.Reply(CHAN_X_NOT_IN_USE, ci->name.c_str());
			return;
		}

		if (!source.AccessFor(ci).HasPriv("UNBAN"))
		{
			source.Reply(ACCESS_DENIED);
			return;
		}

		User *u2 = source.GetUser();
		if (params.size() > 1)
			u2 = User::Find(params[1], true);

		if (!u2)
		{
			source.Reply(NICK_X_NOT_IN_USE, params[1].c_str());
			return;
		}

		Log(LOG_COMMAND, source, this, ci) << "to unban " << u2->nick;

		ci->c->Unban(u2, source.GetUser() == u2);
		if (u2 == source.GetUser())
			source.Reply(_("You have been unbanned from \002%s\002."), ci->c->name.c_str());
		else
			source.Reply(_("\002%s\002 has been unbanned from \002%s\002."), u2->nick.c_str(), ci->c->name.c_str());

		return;
	}

	bool OnHelp(CommandSource &source, const Anope::string &subcommand) anope_override
	{
		this->SendSyntax(source);
		source.Reply(" ");
		source.Reply(_("Tells %s to remove all bans preventing you or the given\n"
				"user from entering the given channel.\n"
				" \n"
				"By default, limited to AOPs or those with level 5 and above\n"
				"on the channel."), source.service->nick.c_str());
		return true;
	}
};

class CSUnban : public Module
{
	CommandCSUnban commandcsunban;

 public:
	CSUnban(const Anope::string &modname, const Anope::string &creator) : Module(modname, creator, CORE),
		commandcsunban(this)
	{
		this->SetAuthor("Anope");

	}
};

MODULE_INIT(CSUnban)
