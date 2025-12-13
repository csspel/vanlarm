// profiles.cpp
#include "profiles.h"

static ProfileConfig profileTable[] = {
  // värdena här är "baseline", fintrimmas senare:
  // TRAVEL: GPS var 10 s
  { ProfileId::TRAVEL, "TRAVEL", 10 * 1000UL, false, false },

  // PARKED: GPS var 5 min
  { ProfileId::PARKED, "PARKED", 5 * 60 * 1000UL, false, false },

  // ALARM: GPS var 10 min, båda PIR aktiva
  { ProfileId::ALARM,  "ALARM", 10 * 60 * 1000UL, true,  true  },

  // STOLEN: GPS var 30 s (och i framtiden TELEMETRY var 60 s)
  { ProfileId::STOLEN, "STOLEN", 30 * 1000UL, false, false }
};

static ProfileId currentId = ProfileId::PARKED;

static const ProfileConfig& findProfile(ProfileId id) {
  for (auto &p : profileTable) {
    if (p.id == id) return p;
  }
  // fallback – ska aldrig hända
  return profileTable[0];
}

void profilesInit(ProfileId defaultProfile) {
  currentId = defaultProfile;
}

const ProfileConfig& currentProfile() {
  return findProfile(currentId);
}

void setProfile(ProfileId id) {
  currentId = id;
}

const char* profileName(ProfileId id) {
  return findProfile(id).name;
}

bool profileFromString(const String& s, ProfileId& out) {
  String up = s;
  up.toUpperCase();

  if (up == "TRAVEL") { out = ProfileId::TRAVEL; return true; }
  if (up == "PARKED") { out = ProfileId::PARKED; return true; }
  if (up == "ALARM")  { out = ProfileId::ALARM;  return true; }
  if (up == "STOLEN") { out = ProfileId::STOLEN; return true; }
  return false;
}
