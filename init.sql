CREATE TYPE user_role AS ENUM ('participant', 'judge');
CREATE TYPE race_status AS ENUM ('not_started', 'in_progress', 'finished');

CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
    surname TEXT NOT NULL,
    login TEXT NOT NULL,
    password_hash TEXT NOT NULL,
    role user_role NOT NULL,
    tg_chat_id BIGINT
);

CREATE TABLE races (
    id SERIAL PRIMARY KEY,
    status race_status NOT NULL DEFAULT 'not_started',
    start_of_the_race TIMESTAMP,
    end_of_the_race TIMESTAMP
);

CREATE TABLE participations (
    user_id BIGINT NOT NULL,
    race_id BIGINT NOT NULL,
    role user_role NOT NULL,
    PRIMARY KEY (user_id, race_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (race_id) REFERENCES races(id) ON DELETE CASCADE
);
