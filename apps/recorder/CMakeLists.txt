spear_add_app(
  ID recorder
  NAME "IQ RECORDER"
  DESCRIPTION "Spectrum + lossless SigMF recording to disk"
  DIRECTION RX
  CLASS spear::apps::RecorderApp
  HEADER recorder_app.hpp
  PAGE RecorderPage.qml
  SOURCES recorder_app.cpp
  LIBS spear_app_spectrum
)
