-- SPDX-License-Identifier: AGPL-3.0-only
-- Copyright (C) 2026 Cyb and contributors. Developed by Cyb; thanks to Hatemost and CoA (Creation of Ashes).
-- GNU AGPL v3 with additional terms under section 7: see LICENSE and NOTICE.
-- Plan B1: the character's race, gender and class in columns of their own, as the client's enums
-- (ECharacterRace, ECharacterGender, ECharacterClass; services/internal/chardef).
--
-- Until now race_id held the CLASS (the services read field 4 of the client's CreateCharacterInfo,
-- which is primary_class: Bard 2, Tank 7, Mage 8) and gender_id the ids 1 (Male) and 2 (Female).
-- What this migration does:
--   * adds race (0 Kaelar ... 7 Nikua, 8 Tulnar), gender (0 Male, 1 Female) and class_id (1 Ranger ... 8 Mage).
--     NULL means "not derived": the services read NULL as 0 and the rows are listed in the warnings below;
--   * fills them where they are still NULL: race and gender from the names the client wrote into the
--     customization JSON (appearance: "race":"Kaelar", "gender":"Male"), the class from class_name when that
--     names a class (the services wrote it from the same payload, and it is the class's own name) and otherwise
--     from the old race_id (a class id when it is 1-8); a row where the two disagree takes class_name's class
--     and is logged;
--   * KEEPS race_id and gender_id exactly as they were: they are the backup of the old values. Nothing reads or
--     writes them any more (their defaults are 0, so an insert may leave them out); a later migration may drop them;
--   * never overwrites a value (every UPDATE is WHERE ... IS NULL), so it can be run again, and it is safe on a
--     table the new services have already written to;
--   * leaves a row whose JSON lacks a race or gender name with NULL there (nothing is guessed), and logs it: one
--     WARNING per such row, a NOTICE with the counts. The services show messages starting with "migration " in
--     their log (internal/db).
-- Nothing else is touched; the whole file runs in the migration's own transaction. lock_timeout makes a table that
-- stays locked fail the start-up with an error instead of waiting for ever.

SET LOCAL lock_timeout = '10s';

ALTER TABLE characters
    ADD COLUMN IF NOT EXISTS race     INT CONSTRAINT characters_race_enum   CHECK (race BETWEEN 0 AND 8),
    ADD COLUMN IF NOT EXISTS gender   INT CONSTRAINT characters_gender_enum CHECK (gender BETWEEN 0 AND 1),
    ADD COLUMN IF NOT EXISTS class_id INT CONSTRAINT characters_class_enum  CHECK (class_id BETWEEN 1 AND 8);

ALTER TABLE characters
    ALTER COLUMN race_id   SET DEFAULT 0,
    ALTER COLUMN gender_id SET DEFAULT 0;

COMMENT ON COLUMN characters.race IS 'ECharacterRace: 0 Kaelar, 1 Vaelune, 2 Vek, 3 Renkai, 4 Empyrean, 5 Pyrai, 6 Dunir, 7 Nikua, 8 Tulnar (never created). NULL: not derived.';
COMMENT ON COLUMN characters.gender IS 'ECharacterGender: 0 Male, 1 Female. NULL: not derived.';
COMMENT ON COLUMN characters.class_id IS 'ECharacterClass: 1 Ranger, 2 Bard, 3 Cleric, 4 Fighter, 5 Rogue, 6 Summoner, 7 Tank, 8 Mage. NULL: not derived.';
COMMENT ON COLUMN characters.race_id IS 'LEGACY (plan B1), kept as the backup: held the CLASS id of the create payload (field 4). Not read or written any more.';
COMMENT ON COLUMN characters.gender_id IS 'LEGACY (plan B1), kept as the backup: 1 Male, 2 Female. Not read or written any more.';

DO $$
DECLARE
    total    INT;
    n_race   INT;
    n_gender INT;
    n_class  INT;
    n_named  INT;
    r        RECORD;
BEGIN

    WITH names (name, id) AS (VALUES
        ('Kaelar', 0), ('Vaelune', 1), ('Vek', 2), ('Renkai', 3), ('Empyrean', 4),
        ('Pyrai', 5), ('Dunir', 6), ('Nikua', 7), ('Tulnar', 8))
    UPDATE characters c SET race = n.id
    FROM names n
    WHERE c.race IS NULL AND c.appearance ->> 'race' = n.name;
    GET DIAGNOSTICS n_race = ROW_COUNT;

    UPDATE characters c SET gender = CASE c.appearance ->> 'gender' WHEN 'Male' THEN 0 ELSE 1 END
    WHERE c.gender IS NULL AND c.appearance ->> 'gender' IN ('Male', 'Female');
    GET DIAGNOSTICS n_gender = ROW_COUNT;

    -- The class: class_name first, when it names a class; then the old race_id, which held the class id.
    WITH names (name, id) AS (VALUES
        ('Ranger', 1), ('Bard', 2), ('Cleric', 3), ('Fighter', 4),
        ('Rogue', 5), ('Summoner', 6), ('Tank', 7), ('Mage', 8))
    UPDATE characters c SET class_id = n.id
    FROM names n
    WHERE c.class_id IS NULL AND c.class_name = n.name;
    GET DIAGNOSTICS n_named = ROW_COUNT;

    UPDATE characters SET class_id = race_id
    WHERE class_id IS NULL AND race_id BETWEEN 1 AND 8;
    GET DIAGNOSTICS n_class = ROW_COUNT;
    n_class := n_class + n_named;

    SELECT count(*) INTO total FROM characters;
    RAISE NOTICE 'migration 0003: % characters; race derived for %, gender for %, class for % (from race_id or class_name)',
        total, n_race, n_gender, n_class;

    -- What was not derived (nothing is guessed), one warning per character.
    FOR r IN
        SELECT id, name, race IS NULL AS no_race, gender IS NULL AS no_gender, class_id IS NULL AS no_class
        FROM characters
        WHERE race IS NULL OR gender IS NULL OR class_id IS NULL
        ORDER BY created_at, id
    LOOP
        RAISE WARNING 'migration 0003: character % (id %) was left without: % (its customization JSON has no usable race or gender name, or its class is unknown; the old race_id and gender_id are kept)',
            r.name, r.id,
            concat_ws(', ',
                CASE WHEN r.no_race THEN 'race' END,
                CASE WHEN r.no_gender THEN 'gender' END,
                CASE WHEN r.no_class THEN 'class' END);
    END LOOP;

    -- A class whose number the old race_id (which held the class id, 1-8) does not repeat: class_name's class was
    -- taken, and race_id is kept as it was. (A row the new services wrote has race_id 0: never listed.)
    FOR r IN
        SELECT id, name, class_id, class_name, race_id
        FROM characters
        WHERE class_id IS NOT NULL AND race_id BETWEEN 1 AND 8 AND race_id <> class_id
        ORDER BY created_at, id
    LOOP
        RAISE WARNING 'migration 0003: character % (id %) has class % (from its class_name %) but its old race_id, which held the class, says % (race_id is kept)',
            r.name, r.id, r.class_id, r.class_name, r.race_id;
    END LOOP;
END
$$;
