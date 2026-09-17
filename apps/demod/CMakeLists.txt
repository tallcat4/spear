spear_add_app(
  ID demod
  NAME "FM / AM RX"
  DESCRIPTION "Narrow/wide FM and AM demodulation to audio, with channel waterfall"
  DIRECTION RX
  CLASS spear::apps::DemodApp
  HEADER demod_app.hpp
  PAGE DemodPage.qml
  SOURCES demod_app.cpp
)
