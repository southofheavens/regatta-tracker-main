CREATE TYPE user_role AS ENUM ('Participant', 'Judge');
CREATE TYPE race_status AS ENUM ('Not_started', 'In_progress', 'Finished');

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
    status race_status NOT NULL DEFAULT 'Not_started',
    start_of_the_race TIMESTAMP NOT NULL,
    end_of_the_race TIMESTAMP NOT NULL
);

CREATE TABLE participations (
    user_id BIGINT NOT NULL,
    race_id BIGINT NOT NULL,
    PRIMARY KEY (user_id, race_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (race_id) REFERENCES races(id) ON DELETE CASCADE
);
