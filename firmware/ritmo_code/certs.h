#pragma once

// Bundle di root CA per gli endpoint HTTPS (api.anthropic.com, status.claude.com).
// Piu' radici per sopravvivere a una rotazione della CA lato server:
//   GlobalSign Root CA      — ancora attuale di api.anthropic.com (scade 2028-01-28)
//   ISRG Root X1            — Let's Encrypt, ancora attuale di status.claude.com (scade 2035-06-04)
//   DigiCert Global Root G2 — destinazione frequente di rotazione (scade 2038-01-15)
extern const char CA_BUNDLE[];
