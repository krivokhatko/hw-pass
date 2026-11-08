# The output differs by board (sound_out.h): the speaker with the codec, or
# the buzzer. A board without both builds sound.c alone: the stubs.
if(CONFIG_HWPASS_SPEAKER)
    set(out "speaker.c")
elseif(CONFIG_HWPASS_BUZZER)
    set(out "buzzer.c")
else()
    set(out "")
endif()

idf_component_register(SRCS "sound.c" ${out}
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_i2s esp_driver_i2c esp_driver_ledc i2cbus pm1)
