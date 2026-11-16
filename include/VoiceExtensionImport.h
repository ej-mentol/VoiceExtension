#pragma once

#ifndef VOICE_EXTENSION_IMPORT_H
#define VOICE_EXTENSION_IMPORT_H

#include "Interface/IVoiceExtension.h"

void VoiceExtension_Init();
void VoiceExtension_Shutdown();
IVoiceExtension* VoiceExtension();
IVoiceExtension2* VoiceExtension2();
IVoiceExtension3* VoiceExtension3();

#endif // VOICE_EXTENSION_IMPORT_H
