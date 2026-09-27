#pragma once

// Bundle di root CA per gli endpoint HTTPS (api.anthropic.com, status.claude.com, GitHub).
// Piu' radici per sopravvivere a una rotazione della CA lato server:
//   GlobalSign Root CA      — ancora attuale di api.anthropic.com (scade 2028-01-28)
//   ISRG Root X1            — Let's Encrypt, ancora attuale di status.claude.com (scade 2035-06-04)
//   DigiCert Global Root G2 — destinazione frequente di rotazione (scade 2038-01-15)
//   USERTrust ECC / RSA     — api.github.com, per gli aggiornamenti del firmware (scadono 2038-01-18)
//   (i file delle release arrivano da release-assets.githubusercontent.com: ISRG Root X1)
//   GTS Root R1 / R4        — Google: api.anthropic.com arriva a GTS Root R4, incrociata con
//                             GlobalSign; senza queste la catena corta falliva "a volte" con
//                             "X509 - Certificate verification failed" (24-09-2026, scadono 2036-06-22)
extern const char CA_BUNDLE[];
