/* OperServ core functions
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

class CommandOSSVSNick : public Command
{
 public:
	CommandOSSVSNick(Module *creator) : Command(creator, "operserv/svsnick", 2, 2)
	{
		this->SetDesc(_("Forcefully change a user's nickname"));
		this->SetSyntax(_("\037nick\037 \037newnick\037"));
	}

	void Execute(CommandSource &source, const std::vector<Anope::string> &params) anope_override
	{
		const Anope::string &nick = params[0];
		Anope::string newnick = params[1];
		User *u2;

		/* Truncate long nicknames to Config->NickLen characters */
		if (newnick.length() > Config->NickLen)
		{
			source.Reply(_("Nick \002%s\002 was truncated to %d characters."), newnick.c_str(), Config->NickLen, newnick.c_str());
			newnick = params[1].substr(0, Config->NickLen);
		}

		/* Check for valid characters */
		if (!IRCD->IsNickValid(newnick))
		{
			source.Reply(_("Nick \002%s\002 is an illegal nickname and cannot be used."), newnick.c_str());
			return;
		}

		/* Check for a nick in use or a forbidden/suspended nick */
		if (!(u2 = User::Find(nick, true)))
			source.Reply(NICK_X_NOT_IN_USE, nick.c_str());
		else if (!nick.equals_ci(newnick) && User::Find(newnick))
			source.Reply(_("Nick \002%s\002 is currently in use."), newnick.c_str());
		else
		{
			source.Reply(_("The nick \002%s\002 is now being changed to \002%s\002."), nick.c_str(), newnick.c_str());
			Log(LOG_ADMIN, source, this) << "to change " << nick << " to " << newnick;
			IRCD->SendForceNickChange(u2, newnick, Anope::CurTime);
		}
		return;
	}

	bool OnHelp(CommandSource &source, const Anope::string &subcommand) anope_override
	{
		this->SendSyntax(source);
		source.Reply(" ");
		source.Reply(_("Forcefully changes a user's nickname from nick to newnick."));
		return true;
	}
};

class OSSVSNick : public Module
{
	CommandOSSVSNick commandossvsnick;

 public:
	OSSVSNick(const Anope::string &modname, const Anope::string &creator) : Module(modname, creator, CORE),
		commandossvsnick(this)
	{
		this->SetAuthor("Anope");

		if (!IRCD || !IRCD->CanSVSNick)
			throw ModuleException("Your IRCd does not support SVSNICK");

	}
};

MODULE_INIT(OSSVSNick)
