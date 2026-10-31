// Glue between the settings table and the engines (slow-motion amplifier, launch boost, skill capture).
#pragma once
void FeaturesInit();       // load [Cinematic] + legacy [TimeAmp] keys, register the live-apply handler, apply once (installs hooks only for enabled features)
void FeaturesApply();      // re-sync every engine with the settings (called after each settings change)
void FeaturesShutdown();   // restore everything the boost changed (quit-bridge / unload)
