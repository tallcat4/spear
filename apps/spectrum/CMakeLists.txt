spear_add_app(
  ID spectrum
  NAME "SPECTRUM"
  DESCRIPTION "Spectrum analyzer + waterfall on radio.rx"
  DIRECTION RX
  CLASS spear::apps::SpectrumApp
  HEADER spectrum_app.hpp
  PAGE SpectrumPage.qml
  SOURCES spectrum_app.cpp
)
