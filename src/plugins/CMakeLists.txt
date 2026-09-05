datamosh_add_plugin( Datamosh
    DESCRIPTION "Datamosh - FFGL motion-driven datamosh effect"
    SOURCES DatamoshPlugin.h DatamoshEffect.h DatamoshEffect.cpp Thumbnail.h
)

datamosh_add_plugin( DatamoshTransplant
    DESCRIPTION "Mosh Transplant - FFGL blend mode, one layer's motion on another"
    SOURCES DatamoshPlugin.h DatamoshMixer.h DatamoshMixer.cpp Thumbnail.h
)
