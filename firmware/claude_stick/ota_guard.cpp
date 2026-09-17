// Ritorno automatico al firmware precedente dopo un aggiornamento via Wi-Fi.
//
// Il bootloader ha il rollback attivo: un firmware appena caricato parte in stato
// "da verificare". Il core Arduino lo conferma subito all'avvio, a meno che questa
// funzione non restituisca true: in quel caso la conferma la da' ota_health_tick()
// nel .ino, solo dopo che il dispositivo gira stabile. Se prima si blocca o si
// riavvia, al boot successivo il bootloader torna da solo alla versione di prima.
#include <Arduino.h>

extern "C" bool verifyRollbackLater() { return true; }
