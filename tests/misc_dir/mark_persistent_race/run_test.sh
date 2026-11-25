leanc ${LEANC_OPTS-} -O3 -DNDEBUG -o mark_persistent_race.produced mark_persistent_race.c
run ./mark_persistent_race.produced
