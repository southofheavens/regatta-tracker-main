CREATE TYPE user_role AS ENUM ('Participant', 'Judge');

CREATE TABLE users (
    id SERIAL PRIMARY KEY,
    name TEXT NOT NULL,
	surname TEXT NOT NULL,
	login TEXT NOT NULL,
	password_hash TEXT NOT NULL,
	role USER_ROLE NOT NULL,
	tg_chat_id BIGINT
);

