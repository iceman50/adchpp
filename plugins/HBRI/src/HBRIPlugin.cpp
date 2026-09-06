/*
 * Copyright (C) 2026 iceman50
 * Licensed under GPL-3.0-or-later.
 */

#include "stdinc.h"
#include "HBRIManager.h"

#include <adchpp/PluginManager.h>
#include <adchpp/version.h>

#ifdef _WIN32
BOOL APIENTRY DllMain(HANDLE, DWORD, LPVOID) {
	return TRUE;
}
#endif

extern "C" {

int PLUGIN_API pluginGetVersion() {
	return PLUGINVERSION;
}

int PLUGIN_API pluginLoad(PluginManager* pm) {
	auto manager = make_shared<HBRIManager>(pm->getCore());
	if(!manager->init()) {
		return 1;
	}
	return pm->registerPlugin("HBRIManager", manager) ? 0 : 2;
}

void PLUGIN_API pluginUnload() {
}

}
