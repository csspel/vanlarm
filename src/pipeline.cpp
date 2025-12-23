#include "pipeline.h"

#include "config.h"
#include "logging.h"
#include "modem.h"
#include "mqtt.h"
#include "gps.h"
#include "time_manager.h"
#include "profiles.h"

// ==============================
// PIR Outbox (server-ack driven)
// ==============================

struct PirOutbox
{
    bool pending = false;
    bool acked = false;
    uint32_t event_id = 0;
    uint16_t count = 0;
    uint32_t first_ms = 0;
    uint32_t last_ms = 0;
    uint8_t src_mask = 0; // bit0=front, bit1=back
};
static PirOutbox g_pir;

static volatile uint16_t g_pirIsrCount = 0;
static volatile bool g_pirIsrFlag = false;

static volatile uint8_t g_pirIsrMask = 0; // bit0=front, bit1=back

static uint32_t g_nextEventId = 1; // TODO: om du vill: persist i NVS/SD senare

static bool g_alarmGpsSkipUsed = false; // skip GPS EN gång per ALARM-episod

static void IRAM_ATTR isrPir()
{
    g_pirIsrCount++;
    g_pirIsrFlag = true;
}

static void IRAM_ATTR isrPirFront()
{
    g_pirIsrCount++;
    g_pirIsrMask |= 0x01;
}

static void IRAM_ATTR isrPirBack()
{
    g_pirIsrCount++;
    g_pirIsrMask |= 0x02;
}

static void pirIngestIsr(uint32_t nowMs)
{
    uint16_t n;
    uint8_t mask;

    noInterrupts();
    n = g_pirIsrCount;
    mask = g_pirIsrMask;
    g_pirIsrCount = 0;
    g_pirIsrMask = 0;
    interrupts();

    if (n == 0)
        return;

    if (!g_pir.pending)
    {
        g_pir.pending = true;
        g_pir.acked = false;
        g_pir.event_id = g_nextEventId++;
        g_pir.count = 0;
        g_pir.first_ms = nowMs;
        g_pir.src_mask = 0; // <-- lägg till i PirOutbox (se nedan)
    }

    g_pir.count = (uint16_t)(g_pir.count + n);
    g_pir.last_ms = nowMs;
    g_pir.src_mask |= mask;
}

// ==============================
// Pipeline state machine
// ==============================

enum class Step
{
    STEP_DECIDE = 0,
    STEP_GPS_ON,
    STEP_GPS_WARMUP,
    STEP_GPS_COLLECT,
    STEP_GPS_OFF,
    STEP_RF_ON,
    STEP_NET_ATTACH,
    STEP_MQTT_CONNECT,
    STEP_PUBLISH,
    STEP_RX_DOWNLINK,
    STEP_MQTT_DISCONNECT,
    STEP_RF_OFF,
    STEP_ALARM_WAIT,
    STEP_PARKED_WAIT
};

static Step g_step = Step::STEP_DECIDE;
static uint32_t g_stepEnterMs = 0;
static uint32_t g_deadlineMs = 0;

static uint32_t g_nextCommAtMs = 0;

static bool g_needComm = false;

// GPS result for this cycle
static bool g_gpsHave = false;
static bool g_gpsFixOk = false;
static GpsFix g_gpsFix;

enum class GpsPlan
{
    NONE = 0,
    SINGLE
};
static GpsPlan g_gpsPlan = GpsPlan::NONE;
static uint32_t g_gpsCollectTimeoutMs = 0;

static void stepEnter(Step s, uint32_t nowMs)
{
    g_step = s;
    g_stepEnterMs = nowMs;

    switch (s)
    {
    case Step::STEP_DECIDE:
        // nothing
        break;

    case Step::STEP_GPS_ON:
        modemRfOff(); // GNSS <-> LTE mux: RF OFF innan GPS
        gpsPowerOn();
        g_deadlineMs = nowMs + 2000UL;
        break;

    case Step::STEP_GPS_WARMUP:
        g_deadlineMs = nowMs + 1500; // 1.5 s settle time
        break;

    case Step::STEP_GPS_COLLECT:
        g_deadlineMs = nowMs + g_gpsCollectTimeoutMs;
        break;

    case Step::STEP_GPS_OFF:
        gpsPowerOff();
        g_deadlineMs = nowMs + 200UL;
        break;

    case Step::STEP_RF_ON:
        gpsPowerOff(); // säkerställ GNSS OFF innan RF
        // modemRfOn();  // <-- du har redan denna funktion
        g_deadlineMs = nowMs + 200UL;
        break;

    case Step::STEP_NET_ATTACH:
        g_deadlineMs = nowMs + 60000UL;
        break;

    case Step::STEP_MQTT_CONNECT:
        g_deadlineMs = nowMs + 15000UL;
        break;

    case Step::STEP_PUBLISH:
        g_deadlineMs = nowMs + 8000UL;
        break;

    case Step::STEP_RX_DOWNLINK:
        g_deadlineMs = nowMs + 5000UL; // kort rx-fönster för ack/downlink
        break;

    case Step::STEP_MQTT_DISCONNECT:
        mqttDisconnect();
        g_deadlineMs = nowMs + 500UL;
        break;

    case Step::STEP_RF_OFF:
        modemRfOff();
        g_deadlineMs = nowMs + 500UL;
        break;

    case Step::STEP_ALARM_WAIT:
        // max 4 min (men vi bryter direkt vid PIR)
        g_deadlineMs = nowMs + 4UL * 60UL * 1000UL;
        break;

    case Step::STEP_PARKED_WAIT:
        // vänta till nästa comm
        g_deadlineMs = g_nextCommAtMs;
        break;
    }
}

static bool stepTimedOut(uint32_t nowMs)
{
    return (int32_t)(nowMs - g_deadlineMs) >= 0;
}

// ==============================
// Hooks från andra moduler
// ==============================

void pipelineOnPirAck(uint32_t eventId)
{
    if (g_pir.pending && g_pir.event_id == eventId)
    {
        g_pir.acked = true;
    }
}

void pipelineOnProfileChanged(ProfileId newProfile)
{
    // När profilen byts: nollställ “skip GPS en gång”
    // (din regel: nollställ vid profilbyte, så nästa ALARM-episod kan skippa igen)
    g_alarmGpsSkipUsed = false;

    // Om du vill vara stenhård: också nollställ scheman
    (void)newProfile;
}

// ==============================
// Init + tick
// ==============================

void pipelineInit()
{
    // Starta med RF & GPS av
    modemRfOff();
    gpsPowerOff();

    g_nextCommAtMs = millis() + 2000UL;

    // PIR pin: du måste sätta detta i config.h
    pinMode(PIN_PIR_FRONT, INPUT);
    pinMode(PIN_PIR_BACK, INPUT);

    int mode = PIR_RISING_EDGE ? RISING : FALLING;
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_FRONT), isrPirFront, mode);
    attachInterrupt(digitalPinToInterrupt(PIN_PIR_BACK), isrPirBack, mode);
    stepEnter(Step::STEP_DECIDE, millis());
}

void pipelineTick(uint32_t nowMs)
{
    // Ingest PIR varje tick (oavsett step)
    pirIngestIsr(nowMs);

    switch (g_step)
    {

    // ---------------- DECIDE ----------------
    case Step::STEP_DECIDE:
    {
        const auto &p = currentProfile();

        // Behöver vi kommunicera?
        bool commDue = ((int32_t)(nowMs - g_nextCommAtMs) >= 0);
        g_needComm = g_pir.pending || commDue;

        // Reset per cycle gps result
        g_gpsHave = false;
        g_gpsFixOk = false;

        // GPS-plan
        g_gpsPlan = GpsPlan::NONE;
        g_gpsCollectTimeoutMs = 0;

        if (g_needComm)
        {
            if (p.id == ProfileId::ALARM)
            {
                // Skip GPS exakt en gång per ALARM-episod (när PIR pending första gången)
                if (g_pir.pending && !g_alarmGpsSkipUsed)
                {
                    g_gpsPlan = GpsPlan::NONE;
                    g_alarmGpsSkipUsed = true;
                }
                else
                {
                    g_gpsPlan = (p.gpsFixWaitMs > 0) ? GpsPlan::SINGLE : GpsPlan::NONE;
                    g_gpsCollectTimeoutMs = (p.gpsFixWaitMs > 0) ? p.gpsFixWaitMs : 0;
                }
            }
            else
            {
                // PARKED/TRAVEL: single fix om fixWaitMs > 0
                g_gpsPlan = (p.gpsFixWaitMs > 0) ? GpsPlan::SINGLE : GpsPlan::NONE;
                g_gpsCollectTimeoutMs = (p.gpsFixWaitMs > 0) ? p.gpsFixWaitMs : 0;
            }
        }

        if (!g_needComm)
        {
            // Vänta enligt profil
            if (p.id == ProfileId::ALARM)
                stepEnter(Step::STEP_ALARM_WAIT, nowMs);
            else if (p.id == ProfileId::PARKED)
                stepEnter(Step::STEP_PARKED_WAIT, nowMs);
            else
                stepEnter(Step::STEP_PARKED_WAIT, nowMs); // TRAVEL: enkelhet, periodisk comm
            break;
        }

        // Kör pipeline
        if (g_gpsPlan != GpsPlan::NONE)
            stepEnter(Step::STEP_GPS_ON, nowMs);
        else
            stepEnter(Step::STEP_RF_ON, nowMs);

        break;
    }

    // ---------------- GPS ----------------
    case Step::STEP_GPS_ON:
    {
        // inga krav, gå vidare snabbt
        stepEnter(Step::STEP_GPS_WARMUP, nowMs);

        break;
    }

    case Step::STEP_GPS_WARMUP:
        if (stepTimedOut(nowMs))
        {
            stepEnter(Step::STEP_GPS_COLLECT, nowMs);
        }
        break;

    case Step::STEP_GPS_COLLECT:
    {
        GpsFix fx;
        bool ok = gpsGetFixWait(fx, 2000); // små “slices”, ej stor block
        if (ok)
        {
            g_gpsFix = fx;
            g_gpsFixOk = true;
            g_gpsHave = true;
            stepEnter(Step::STEP_GPS_OFF, nowMs);
            break;
        }

        if (stepTimedOut(nowMs))
        {
            // timeout: fortsätt ändå utan GPS
            g_gpsHave = false;
            g_gpsFixOk = false;
            stepEnter(Step::STEP_GPS_OFF, nowMs);
        }
        break;
    }

    case Step::STEP_GPS_OFF:
    {
        stepEnter(Step::STEP_RF_ON, nowMs);
        break;
    }

    // ---------------- RF + NET + MQTT ----------------
    case Step::STEP_RF_ON:
    {
        stepEnter(Step::STEP_NET_ATTACH, nowMs);
        break;
    }

    case Step::STEP_NET_ATTACH:
    {
        NetResult net;
        bool ok = modemConnectData(APN, NET_REG_TIMEOUT_MS, DATA_ATTACH_TIMEOUT_MS, net);

        if (ok)
        {
            // sync time best effort (du har redan dessa funktioner)
            timeSyncFromModem();
            timeSyncFromNtp(8000);
            stepEnter(Step::STEP_MQTT_CONNECT, nowMs);
            break;
        }

        if (stepTimedOut(nowMs))
        {
            // backoff: planera nästa försök
            const auto &p = currentProfile();
            g_nextCommAtMs = nowMs + p.commIntervalMs;
            stepEnter(Step::STEP_RF_OFF, nowMs);
        }
        break;
    }

    case Step::STEP_MQTT_CONNECT:
    {
        if (mqttConnect())
        {

            stepEnter(Step::STEP_PUBLISH, nowMs);
            break;
        }

        if (stepTimedOut(nowMs))
        {
            const auto &p = currentProfile();
            g_nextCommAtMs = nowMs + p.commIntervalMs;
            stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
        }
        break;
    }

    case Step::STEP_PUBLISH:
    {
        // GPS (single)
        if (g_gpsHave)
        {
            mqttPublishGpsSingle(g_gpsFix, g_gpsFixOk);
        }

        // PIR event (outbox, rensas INTE här)
        if (g_pir.pending)
        {
            mqttPublishPirEvent(g_pir.event_id, g_pir.count, g_pir.first_ms, g_pir.last_ms, g_pir.src_mask);
        }

        // Alive
        mqttPublishAlive();

        stepEnter(Step::STEP_RX_DOWNLINK, nowMs);
        break;
    }

    case Step::STEP_RX_DOWNLINK:
    {
        mqttLoop();
        if (stepTimedOut(nowMs))
        {
            stepEnter(Step::STEP_MQTT_DISCONNECT, nowMs);
        }
        break;
    }

    case Step::STEP_MQTT_DISCONNECT:
    {
        stepEnter(Step::STEP_RF_OFF, nowMs);
        break;
    }

    case Step::STEP_RF_OFF:
    {
        // Rensa PIR endast på server-ack
        if (g_pir.pending && g_pir.acked)
        {
            logSystem("PIR: event acked, clearing outbox");
            g_pir.pending = false;
            g_pir.acked = false;
            g_pir.count = 0;
        }
        else if (g_pir.pending)
        {
            // failsafe: låt inte PIR blockera GPS för evigt
            logSystem("PIR: pending without ack (will retry later)");
        }

        // Schemalägg nästa comm enligt profil
        const auto &p = currentProfile();
        g_nextCommAtMs = nowMs + p.commIntervalMs;

        // Gå till WAIT enligt profil
        if (p.id == ProfileId::ALARM)
            stepEnter(Step::STEP_ALARM_WAIT, nowMs);
        else if (p.id == ProfileId::PARKED)
            stepEnter(Step::STEP_PARKED_WAIT, nowMs);
        else
            stepEnter(Step::STEP_PARKED_WAIT, nowMs);
        break;
    }

    // ---------------- WAIT ----------------
    case Step::STEP_ALARM_WAIT:
    {
        // Bryt direkt om PIR triggar igen (outbox pending)
        if (g_pir.pending)
        {
            // force immediate comm
            g_nextCommAtMs = nowMs;
            stepEnter(Step::STEP_DECIDE, nowMs);
            break;
        }
        // timeout 4 min: ingen heartbeat – bara “omvärdera”
        if (stepTimedOut(nowMs))
        {
            stepEnter(Step::STEP_DECIDE, nowMs);
        }
        break;
    }

    case Step::STEP_PARKED_WAIT:
    {
        if ((int32_t)(nowMs - g_nextCommAtMs) >= 0)
        {
            stepEnter(Step::STEP_DECIDE, nowMs);
        }
        break;
    }
    }
}
