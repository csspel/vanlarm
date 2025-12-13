// profiles.h
#pragma once
#include <Arduino.h>

enum class ProfileId {
  TRAVEL,
  PARKED,
  ALARM,
  STOLEN
};

struct ProfileConfig {
  ProfileId   id;
  const char* name;           // "TRAVEL", "PARKED", ...
  uint32_t    gpsIntervalMs;  // standard-intervall för GPS (kan justeras sen)
  bool        pirFront;       // PIR fram aktiv?
  bool        pirBack;        // PIR bak aktiv?
};

void profilesInit(ProfileId defaultProfile);
const ProfileConfig& currentProfile();
void setProfile(ProfileId id);

const char* profileName(ProfileId id);
bool profileFromString(const String& s, ProfileId& out);
