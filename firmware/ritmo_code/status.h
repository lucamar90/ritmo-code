#pragma once

// Salute dei modelli via status.claude.com. Gli incidenti non risolti citano la
// famiglia del modello nel testo ("Elevated errors on Claude Opus 4.6"), quindi una
// ricerca per parola chiave e' tutto il parsing necessario.
struct ModelStatus {
    bool haikuUp;
    bool sonnetUp;
    bool opusUp;
    bool fableUp;
    bool ok;       // true dopo che almeno un fetch e' riuscito
};

bool fetchModelStatus(ModelStatus& out);
