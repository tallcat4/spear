spear_add_app(
  ID replay
  NAME "IQ REPLAY TX"
  DESCRIPTION "Transmit a recording as recorded (needs TX MASTER ARMED)"
  DIRECTION TX
  CLASS spear::apps::ReplayApp
  HEADER replay_app.hpp
  PAGE ReplayPage.qml
  SOURCES replay_app.cpp
)
