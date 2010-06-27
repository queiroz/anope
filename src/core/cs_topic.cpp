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

class CommandCSTopic : public Command
{
 public:
	CommandCSTopic() : Command("TOPIC", 1, 2)
	{
	}

	CommandReturn Execute(User *u, const std::vector<ci::string> &params)
	{
		const char *chan = params[0].c_str();
		const char *topic = params.size() > 1 ? params[1].c_str() : NULL;

		Channel *c;
		ChannelInfo *ci;

		if ((c = findchan(chan)))
			ci = c->ci;

		if (!c)
			notice_lang(Config.s_ChanServ, u, CHAN_X_NOT_IN_USE, chan);
		else if (!check_access(u, ci, CA_TOPIC) && !u->Account()->HasCommand("chanserv/topic"))
			notice_lang(Config.s_ChanServ, u, ACCESS_DENIED);
		else
		{
			if (ci->last_topic)
				delete [] ci->last_topic;
			ci->last_topic = topic ? sstrdup(topic) : NULL;
			ci->last_topic_setter = u->nick;
			ci->last_topic_time = time(NULL);

			if (c->topic)
				delete [] c->topic;
			c->topic = topic ? sstrdup(topic) : NULL;
			c->topic_setter = u->nick;
			if (ircd->topictsbackward)
				c->topic_time = c->topic_time - 1;
			else
				c->topic_time = ci->last_topic_time;

			if (!check_access(u, ci, CA_TOPIC))
				Alog() << Config.s_NickServ << ": " << u->GetMask() << " changed topic of " << c->name << " as services admin.";
			if (ircd->join2set && whosends(ci) == ChanServ)
			{
				ircdproto->SendJoin(ChanServ, c->name.c_str(), c->creation_time);
				ircdproto->SendMode(NULL, c, "+o %s", Config.s_ChanServ);
			}
			ircdproto->SendTopic(whosends(ci), c, u->nick.c_str(), topic ? topic : "");
			if (ircd->join2set && whosends(ci) == ChanServ)
				ircdproto->SendPart(ChanServ, c, NULL);
		}
		return MOD_CONT;
	}

	bool OnHelp(User *u, const ci::string &subcommand)
	{
		notice_help(Config.s_ChanServ, u, CHAN_HELP_TOPIC);
		return true;
	}

	void OnSyntaxError(User *u, const ci::string &subcommand)
	{
		syntax_error(Config.s_ChanServ, u, "TOPIC", CHAN_TOPIC_SYNTAX);
	}

	void OnServHelp(User *u)
	{
		notice_lang(Config.s_ChanServ, u, CHAN_HELP_CMD_TOPIC);
	}
};

class CSTopic : public Module
{
 public:
	CSTopic(const std::string &modname, const std::string &creator) : Module(modname, creator)
	{
		this->SetAuthor("Anope");
		this->SetType(CORE);

		this->AddCommand(ChanServ, new CommandCSTopic());
	}
};

MODULE_INIT(CSTopic)