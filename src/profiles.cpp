// profiles.cpp
#include "profiles.h"

// En enda källa för alla intervall.
// Justera här – så följer main.cpp automatiskt med.
static ProfileConfig profileTable[] = {
    // TRAVEL: GPS ofta, uplink var 5 min (batch kommer senare)
    {ProfileId::TRAVEL, "TRAVEL", 10 * 1000UL, 5 * 60 * 1000UL, 0, 5 * 1000UL, false},

    // PARKED: GPS single var 5 min, uplink var 5 min
    {ProfileId::PARKED, "PARKED", 5 * 60 * 1000UL, 5 * 60 * 1000UL, 60 * 1000UL, 10 * 1000UL, false},

    // ALARM: GPS single var 5 min (backup om PIR missar), uplink var 5 min
    {ProfileId::ALARM, "ALARM", 5 * 60 * 1000UL, 5 * 60 * 1000UL, 60 * 1000UL, 10 * 1000UL, true},

    // STOLEN: vill spåra — GPS single + uplink tätare
    {ProfileId::STOLEN, "STOLEN", 2 * 60 * 1000UL, 2 * 60 * 1000UL, 60 * 1000UL, 6 * 1000UL, false}};

static ProfileId currentId = ProfileId::PARKED;

static const ProfileConfig &findProfile(ProfileId id)
{
  for (auto &p : profileTable)
  {
    if (p.id == id)
      return p;
  }
  return profileTable[0];
}

void profilesInit(ProfileId defaultProfile)
{
  currentId = defaultProfile;
}

const ProfileConfig &currentProfile()
{
  return findProfile(currentId);
}

void setProfile(ProfileId id)
{
  currentId = id;
  extern void pipelineOnProfileChanged(ProfileId newProfile);
  pipelineOnProfileChanged(id);
}

const char *profileName(ProfileId id)
{
  return findProfile(id).name;
}

bool profileFromString(const String &s, ProfileId &out)
{
  String up = s;
  up.toUpperCase();

  if (up == "TRAVEL")
  {
    out = ProfileId::TRAVEL;
    return true;
  }
  if (up == "PARKED")
  {
    out = ProfileId::PARKED;
    return true;
  }
  if (up == "ALARM")
  {
    out = ProfileId::ALARM;
    return true;
  }
  if (up == "STOLEN")
  {
    out = ProfileId::STOLEN;
    return true;
  }
  return false;
}
