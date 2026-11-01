#ifndef ZHAOXI_EMOTION_DISPLAY_H
#define ZHAOXI_EMOTION_DISPLAY_H

void extensionDisplayBegin();
bool extensionDisplayActive();
bool extensionDisplayTick();
bool extensionDisplayHandleLine(const char *line);
void extensionDisplayProtocolError(const char *error);
void extensionDisplayRelease();
// Background legacy animations must obtain the same matrix ownership lock.
bool extensionLegacyDisplayBegin();
void extensionLegacyDisplayEnd();
void extensionDisplayDeleteTask(void *handle);
// Implemented by the clock task; avoids pulling its large HTML/assets header here.
int extensionDisplayBasePage();

#endif
