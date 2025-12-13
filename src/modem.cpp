#include "modem.h"
#include "config.h"
#include "logging.h"

#include <TinyGsmClient.h>

// UART mot modem
HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
static TinyGsmClient gsmClient(modem);

// ====== interna hjälpfunktioner ======

static bool modemWaitForAT(uint32_t timeoutMs) {
    uint32_t start = millis();
    int retry = 0;

    logSystem("MODEM: waiting for AT...");

    while (millis() - start < timeoutMs) {
        if (modem.testAT(1000)) {
            logSystem("MODEM: AT OK");
            return true;
        }

        retry++;
        logSystem("MODEM: no AT yet, retry=" + String(retry));
        delay(1000);

        // Om vi försökt några gånger: PWRKEY-puls
        if (retry % 6 == 0) {
            logSystem("MODEM: PWRKEY pulse to start modem");
            pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
            digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
            delay(100);
            digitalWrite(BOARD_MODEM_PWR_PIN, HIGH);
            delay(1000);
            digitalWrite(BOARD_MODEM_PWR_PIN, LOW);
        }
    }

    logSystem("MODEM: AT FAILED after " + String(timeoutMs) + " ms");
    return false;
}

static bool modemWaitForSimReady(uint32_t timeoutMs) {
    uint32_t start = millis();

    while (millis() - start < timeoutMs) {
        auto st = modem.getSimStatus();
        if (st == SIM_READY) {
            logSystem("MODEM: SIM ready");
            return true;
        }
        logSystem("MODEM: SIM not ready yet (status=" + String((int)st) + ")");
        delay(1000);
    }

    logSystem("MODEM: SIM still NOT ready after " + String(timeoutMs) + " ms");
    return false;
}

static bool modemWaitForNetwork(uint32_t timeoutMs) {
    uint32_t start = millis();

    logSystem("MODEM: wait for network registration...");

    while (millis() - start < timeoutMs) {
        if (modem.isNetworkConnected()) {
            logSystem("MODEM: network registered");
            return true;
        }
        Serial.print(".");
        delay(1000);
    }

    logSystem("MODEM: network registration TIMEOUT");
    return false;
}

static bool modemActivateData(uint32_t timeoutMs) {
    // Kolla först om vi redan har datalänk
    bool gprsBefore = modem.isGprsConnected();
    logSystem(String("MODEM: GPRS status before CNACT: ") + (gprsBefore ? "connected" : "NOT connected"));

    if (gprsBefore) {
        logSystem("MODEM: GPRS already connected, skip CNACT");
        return true;
    }

    // Försök aktivera data
    logSystem("MODEM: activate data bearer (+CNACT=0,1)");
    modem.sendAT("+CNACT=0,1");
    if (modem.waitResponse(timeoutMs) != 1) {
        logSystem("MODEM: CNACT failed, re-checking GPRS state");

        bool gprsAfter = modem.isGprsConnected();
        logSystem(String("MODEM: GPRS status after CNACT fail: ") + (gprsAfter ? "connected" : "NOT connected"));

        if (gprsAfter) {
            logSystem("MODEM: treating CNACT fail as non-fatal (GPRS is connected)");
            return true;
        }

        // Här ger vi faktiskt upp
        logSystem("MODEM: data attach really FAILED");
        return false;
    }

    // Om CNACT svarade OK, verifiera GPRS för loggens skull
    bool gprsAfter = modem.isGprsConnected();
    logSystem(String("MODEM: GPRS status after CNACT OK: ") + (gprsAfter ? "connected" : "NOT connected"));

    return gprsAfter;  // normalt true, men skulle det vara false loggar vi åtminstone
}


// ====== publika funktioner ======

void modemInitUartAndPins() {
    logSystem("MODEM: init UART & pins");

    // Starta UART mot modemet
    SerialAT.begin(115200, SERIAL_8N1, BOARD_MODEM_RXD_PIN, BOARD_MODEM_TXD_PIN);

    pinMode(BOARD_MODEM_PWR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_DTR_PIN, OUTPUT);
    pinMode(BOARD_MODEM_RI_PIN, INPUT);

    // Håll modem vaken
    digitalWrite(BOARD_MODEM_DTR_PIN, LOW);
}

bool modemConnectData(const char *apn,
                      uint32_t netRegTimeoutMs,
                      uint32_t dataAttachTimeoutMs,
                      NetResult &out)
{
    out.ip  = "";
    out.csq = -1;
    out.err = "";

    uint32_t tStart = millis();

    // 1) Säkerställ AT-kontakt
    if (!modemWaitForAT(30000UL)) {
        out.err = "no_at";
        return false;
    }

    // 2) Försök vänta in SIM READY, men gör det som en *mjuk* check
    if (!modemWaitForSimReady(20000UL)) {
        logSystem("MODEM: SIM check failed, fortsätter ändå (litar på nätuppkopplingstest)");
        // Vi sätter INTE out.err här, vi bara loggar och går vidare
    }

    // 3) Kolla om vi redan är nätregistrerade
    bool alreadyNet = modem.isNetworkConnected();
    if (!alreadyNet) {
        logSystem("MODEM: not network connected → doing full CFUN/APN setup");

        // Stäng av RF (CFUN=0) innan APN-inställning
        logSystem("MODEM: disable RF (CFUN=0)");
        modem.sendAT("+CFUN=0");
        modem.waitResponse(20000UL);  // ignorerar fel, bara loggat

        // nätverksläge + preferred mode
        modem.setNetworkMode(2);   // auto
        modem.setPreferredMode(3); // CAT-M + NB-IoT

        // APN-inställning (CGDCONT + CNCFG)
        logSystem("MODEM: set APN via CGDCONT/CNCFG");
        modem.sendAT("+CGDCONT=1,\"IP\",\"", apn, "\"");
        if (modem.waitResponse(5000UL) != 1) {
            logSystem("MODEM: CGDCONT failed");
        }

        modem.sendAT("+CNCFG=0,1,\"", apn, "\"");
        if (modem.waitResponse(5000UL) != 1) {
            logSystem("MODEM: CNCFG failed");
        }

        // Slå på RF (CFUN=1)
        logSystem("MODEM: enable RF (CFUN=1)");
        modem.sendAT("+CFUN=1");
        if (modem.waitResponse(20000UL) != 1) {
            logSystem("MODEM: CFUN=1 failed");
        }

        // Vänta på nätregistrering
        if (!modemWaitForNetwork(netRegTimeoutMs)) {
            out.err = "net_timeout";
            return false;
        }
    } else {
        logSystem("MODEM: already network connected, reusing registration");
    }

    // 4) Aktivera data (idempotent – kan köras även om det redan är aktivt)
    if (!modemActivateData(dataAttachTimeoutMs)) {
        out.err = "data_attach_failed";
        return false;
    }

    // 5) Verifiera GPRS, IP, CSQ
    bool gprs = modem.isGprsConnected();
    logSystem(String("MODEM: GPRS status: ") + (gprs ? "connected" : "NOT connected"));

    IPAddress ip = modem.localIP();
    out.ip = ip.toString();

    int csq = modem.getSignalQuality();
    out.csq = csq;

    logSystem("MODEM: Local IP: " + out.ip);
    logSystem("MODEM: CSQ: " + String(csq));

    uint32_t tTotal = millis() - tStart;
    logSystem("NET_CONNECT: SUCCESS, T_net=" + String(tTotal) +
              " ms, IP=" + out.ip + ", CSQ=" + String(csq));

    return true;
}

Client& modemGetClient() {
    return gsmClient;
}
