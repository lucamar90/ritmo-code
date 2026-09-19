#include "accounts.h"
#include <string.h>
#include <strings.h>   // strcasecmp
#include <stdio.h>

static void keyName(const char* base, int slot, char* out, size_t sz) {
    snprintf(out, sz, "%s%d", base, slot);
}

static void sanitizeLabel(const char* in, char* out) {
    int n = 0;
    for (const char* c = in; *c && n < ACCT_LBL_MAX - 1; c++) {
        if (*c < 0x20 || *c > 0x7E) continue;
        if (*c == '"' || *c == '\\' || *c == '<' || *c == '>') continue;
        out[n++] = *c;
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

// Garantisce un'etichetta unica tra gli slot in uso, aggiungendo " 2", " 3"... quando
// serve, cosi' due slot non compaiono con lo stesso nome sullo schermo. Il
// confronto ignora maiuscole/minuscole ("Lavoro" e "lavoro" sono uguali
// per chi guarda lo schermo).
static void makeUnique(const AccountSlots& s, int slot, char* lbl) {
    char base[ACCT_LBL_MAX];
    strlcpy(base, lbl, sizeof(base));
    // ci sono al massimo ACCT_MAX-1 etichette concorrenti, quindi un suffisso fino a
    // ACCT_MAX+1 resta sempre libero e il ciclo termina.
    for (int n = 2; n <= ACCT_MAX + 1; n++) {
        bool clash = false;
        for (int i = 0; i < ACCT_MAX; i++)
            if (i != slot && s.used[i] && strcasecmp(s.label[i], lbl) == 0) { clash = true; break; }
        if (!clash) return;
        char trimmed[ACCT_LBL_MAX];
        strlcpy(trimmed, base, ACCT_LBL_MAX - 2);   // riserva spazio per " N"
        snprintf(lbl, ACCT_LBL_MAX, "%s %d", trimmed, n);
    }
}

void accountsLoad(Preferences& p, AccountSlots& s) {
    memset(&s, 0, sizeof(s));

    // Migrazione dal formato ad account singolo: "blob" -> "blob0". Il vecchio "blob" e'
    // conservato DELIBERATAMENTE — fa da copia di rollback se la scheda torna
    // a eseguire un firmware precedente al multi-account (che conosce solo quella chiave).
    // Rimigrare sopra e' impossibile: la condizione richiede "blob0" assente.
    // Costo: ~550 byte di NVS. Chi vuole recuperare lo spazio usa Cancella tutto.
    if (p.getBytesLength("blob") == sizeof(EncryptedBlob) &&
        p.getBytesLength("blob0") != sizeof(EncryptedBlob)) {
        EncryptedBlob b;
        p.getBytes("blob", &b, sizeof(b));
        p.putBytes("blob0", &b, sizeof(b));
        p.putString("lbl0", "Account 1");
        p.putInt("acct", 0);
    }

    char k[8];
    for (int i = 0; i < ACCT_MAX; i++) {
        keyName("blob", i, k, sizeof(k));
        s.used[i] = (p.getBytesLength(k) == sizeof(EncryptedBlob));
        keyName("lbl", i, k, sizeof(k));
        String l = p.getString(k, "");
        strlcpy(s.label[i], l.c_str(), ACCT_LBL_MAX);
        if (s.used[i] && !s.label[i][0])
            snprintf(s.label[i], ACCT_LBL_MAX, "Account %d", i + 1);
    }

    s.active = p.getInt("acct", 0);
    if (s.active < 0 || s.active >= ACCT_MAX) s.active = 0;
    if (!s.used[s.active])
        for (int i = 0; i < ACCT_MAX; i++)
            if (s.used[i]) { s.active = i; break; }
}

bool accountLoadBlob(Preferences& p, int slot, EncryptedBlob& out) {
    if (slot < 0 || slot >= ACCT_MAX) return false;
    char k[8];
    keyName("blob", slot, k, sizeof(k));
    if (p.getBytesLength(k) != sizeof(EncryptedBlob)) return false;
    return p.getBytes(k, &out, sizeof(EncryptedBlob)) == sizeof(EncryptedBlob);
}

void accountSave(Preferences& p, AccountSlots& s, int slot,
                 const EncryptedBlob& blob, const char* label) {
    if (slot < 0 || slot >= ACCT_MAX) return;
    char k[8];
    keyName("blob", slot, k, sizeof(k));
    p.putBytes(k, &blob, sizeof(EncryptedBlob));
    s.used[slot] = true;
    accountSetLabel(p, s, slot, label);
    if (accountCount(s) == 1) accountSetActive(p, s, slot);
}

void accountSetLabel(Preferences& p, AccountSlots& s, int slot, const char* label) {
    if (slot < 0 || slot >= ACCT_MAX || !s.used[slot]) return;
    char clean[ACCT_LBL_MAX];
    sanitizeLabel(label ? label : "", clean);
    if (!clean[0]) snprintf(clean, sizeof(clean), "Account %d", slot + 1);
    makeUnique(s, slot, clean);
    char k[8];
    keyName("lbl", slot, k, sizeof(k));
    p.putString(k, clean);
    strlcpy(s.label[slot], clean, ACCT_LBL_MAX);
}

void accountRemove(Preferences& p, AccountSlots& s, int slot) {
    if (slot < 0 || slot >= ACCT_MAX) return;
    char k[8];
    keyName("blob", slot, k, sizeof(k)); p.remove(k);
    keyName("lbl", slot, k, sizeof(k));  p.remove(k);
    s.used[slot] = false;
    s.label[slot][0] = 0;
    if (s.active == slot) {
        s.active = 0;
        for (int i = 0; i < ACCT_MAX; i++)
            if (s.used[i]) { s.active = i; break; }
        p.putInt("acct", s.active);
    }
}

void accountSetActive(Preferences& p, AccountSlots& s, int slot) {
    if (slot < 0 || slot >= ACCT_MAX || !s.used[slot]) return;
    s.active = slot;
    p.putInt("acct", slot);
}

int accountCount(const AccountSlots& s) {
    int n = 0;
    for (int i = 0; i < ACCT_MAX; i++) if (s.used[i]) n++;
    return n;
}

int accountFirstFree(const AccountSlots& s) {
    for (int i = 0; i < ACCT_MAX; i++) if (!s.used[i]) return i;
    return -1;
}
