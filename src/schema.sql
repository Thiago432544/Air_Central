-- Banco da central (SQLite). Versão 1.
-- O Repositorio tira os comentários e separa os comandos por ponto e vírgula,
-- então não use ponto e vírgula nem dois hífens dentro de textos entre aspas.

-- Modelo de ar-condicionado (marca/modelo do controle).
CREATE TABLE modelos (
    id      INTEGER PRIMARY KEY,
    nome    TEXT    NOT NULL UNIQUE,
    marca   TEXT    NOT NULL DEFAULT '',
    formato TEXT    NOT NULL DEFAULT 'raw' CHECK (formato IN ('raw', 'protocolo'))
);

-- Um código IR por (modelo, comando).
-- comando: LIGAR, DESLIGAR ou T16..T30 (ligar já na temperatura).
CREATE TABLE codigos (
    modelo_id    INTEGER NOT NULL REFERENCES modelos(id) ON DELETE CASCADE,
    comando      TEXT    NOT NULL CHECK (comando IN ('LIGAR', 'DESLIGAR') OR comando GLOB 'T[123][0-9]'),
    dados        TEXT    NOT NULL,
    capturado_em TEXT    NOT NULL DEFAULT (datetime('now')),
    PRIMARY KEY (modelo_id, comando)
);

-- Sala do prédio. modelo_id NULL = modelo ainda não definido.
CREATE TABLE salas (
    id          INTEGER PRIMARY KEY,
    nome        TEXT    NOT NULL UNIQUE,
    bloco       TEXT    NOT NULL DEFAULT '',
    modelo_id   INTEGER REFERENCES modelos(id) ON DELETE SET NULL,
    temp_padrao INTEGER NOT NULL DEFAULT 22 CHECK (temp_padrao BETWEEN 16 AND 30)
);

-- Placa ESP. chip_id é o ID que a placa informa (CI-<chipId> ou o nome
-- dado pelo Change_ID). Uma placa atende no máximo uma sala.
-- hash_config: hash da última config que a placa confirmou ter (8 hex do
-- SHA-1 de {codigos, agenda}, o mesmo de configDaSala). NULL = desconhecido.
CREATE TABLE nos (
    chip_id        TEXT    PRIMARY KEY,
    sala_id        INTEGER UNIQUE REFERENCES salas(id) ON DELETE SET NULL,
    hash_config    TEXT,
    ultimo_contato TEXT,
    estado         TEXT    NOT NULL DEFAULT 'Desconhecido',
    temp           INTEGER
);

-- dia: 0 = Dom ... 6 = Sab (mesmo padrão do tm_wday do nó).
-- origem: 'saci' é substituída a cada importação; 'manual' nunca é tocada;
-- 'legado' veio dos JSON antigos (não dá para saber se era manual ou SACI).
CREATE TABLE agendamentos (
    id      INTEGER PRIMARY KEY,
    sala_id INTEGER NOT NULL REFERENCES salas(id) ON DELETE CASCADE,
    dia     INTEGER NOT NULL CHECK (dia BETWEEN 0 AND 6),
    hora    TEXT    NOT NULL CHECK (hora GLOB '[0-2][0-9]:[0-5][0-9]'),
    acao    TEXT    NOT NULL CHECK (acao IN ('LIGAR', 'DESLIGAR')),
    temp    INTEGER CHECK (temp IS NULL OR temp BETWEEN 16 AND 30),
    origem  TEXT    NOT NULL DEFAULT 'manual' CHECK (origem IN ('manual', 'saci', 'legado')),
    ativo   INTEGER NOT NULL DEFAULT 1 CHECK (ativo IN (0, 1))
);

CREATE INDEX idx_agendamentos_sala ON agendamentos(sala_id, dia, hora);
