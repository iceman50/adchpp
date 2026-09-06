/*
 * Copyright (C) 2026 iceman50
 * Licensed under GPL-3.0-or-later.
 *
 * Implements the hub portion of RTF0 version 0.2 (2026-08-03).
 * The hub negotiates and relays rich text; rendering remains a client concern.
 */

#include "stdinc.h"
#include "RTF0Manager.h"

#include <adchpp/AdcCommand.h>
#include <adchpp/Core.h>
#include <adchpp/Entity.h>
#include <adchpp/File.h>
#include <adchpp/LogManager.h>
#include <adchpp/SimpleXML.h>

using namespace std;
using namespace std::placeholders;
using namespace adchpp;

const string RTF0Manager::className = "RTF0Manager";

namespace {
	const uint32_t FEATURE_RTF0 = AdcCommand::toFourCC("RTF0");
}

RTF0Manager::RTF0Manager(Core& core_) : core(core_), enabled(true) {
	LOG(className, "Starting");
}

RTF0Manager::~RTF0Manager() {
	core.getClientManager().getEntity(AdcCommand::HUB_SID)->removeSupports(FEATURE_RTF0);
	LOG(className, "Shutting down");
}

bool RTF0Manager::loadConfig() {
	const string path = core.getConfigPath() + "RTF0.xml";
	if(File::getSize(path) < 0) {
		LOG(className, "RTF0.xml not found; enabling RTF0 with defaults");
		return true;
	}

	try {
		SimpleXML xml;
		xml.fromXML(File(path, File::READ).read());
		xml.stepIn();
		if(xml.findChild("Enabled")) {
			const string value = xml.getChildData();
			enabled = value == "1" || value == "true" || value == "yes";
		}
		xml.stepOut();
		return true;
	} catch(const Exception& e) {
		LOG(className, "Unable to load RTF0.xml: " + e.getError());
		return false;
	}
}

bool RTF0Manager::init() {
	if(!loadConfig()) {
		return false;
	}

	ClientManager& cm = core.getClientManager();
	receiveConnection = manage(cm.signalReceive().connect(bind(&RTF0Manager::onReceive, this, _1, _2, _3)));
	badLineConnection = manage(cm.signalBadLine().connect(bind(&RTF0Manager::onBadLine, this, _1, _2)));

	if(enabled) {
		cm.getEntity(AdcCommand::HUB_SID)->addSupports(FEATURE_RTF0);
		LOG(className, "RTF0 enabled");
	} else {
		LOG(className, "RTF0 disabled; incoming RT1 flags will be stripped");
	}

	return true;
}

void RTF0Manager::onReceive(Entity&, AdcCommand& command, bool& ok) {
	if(!ok || enabled || command.getCommand() != AdcCommand::CMD_MSG || !command.hasFlag("RT", 1)) {
		return;
	}

	// RTF0 section "Hub handling of a violation" recommends stripping RT1
	// when the hub has not announced the feature. Remove duplicate RT fields too.
	while(command.delParam("RT", 1)) {
	}
}

bool RTF0Manager::recoverUnknownEscapes(const string& line, string& recovered) {
	recovered.clear();
	recovered.reserve(line.size());
	bool changed = false;
	for(size_t i = 0; i < line.size(); ++i) {
		if(line[i] != '\\') {
			recovered += line[i];
			continue;
		}
		if(i + 1 >= line.size()) {
			return false;
		}
		const char escaped = line[i + 1];
		if(escaped == 's' || escaped == 'n' || escaped == '\\') {
			recovered += line[i];
			recovered += escaped;
		} else {
			// RTF0 requires an undefined ADC escape to be interpreted as the
			// escaped character, with the backslash discarded.
			recovered += escaped;
			changed = true;
		}
		++i;
	}
	return changed;
}

void RTF0Manager::onBadLine(Entity& entity, const string& line) {
	if(!enabled) {
		return;
	}

	string recovered;
	if(!recoverUnknownEscapes(line, recovered)) {
		return;
	}

	try {
		AdcCommand command(recovered);
		if(command.getCommand() != AdcCommand::CMD_MSG || !command.hasFlag("RT", 1)) {
			return;
		}
		if(command.getType() == AdcCommand::TYPE_HUB) {
			command.setFrom(entity.getSID());
		} else if(command.getFrom() != entity.getSID()) {
			return;
		}
		entity.inject(command);
	} catch(const ParseException&) {
		// The line had another syntax error as well; leave it rejected.
	}
}
