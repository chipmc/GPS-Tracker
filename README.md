# Simple Site Survey Tool

# Overview and Concept of Operations

This tool provides a simple way to measure the signal strength and associate each reading with a GPS coordinate.  The goal is to anticipate cellular coverage issues and select better locations for sensor placement.  The tool will start connecting, getting a GPS fix and transmitting data every 60 seconds once it is powered on.  You can monitor its progress three ways:
  1) Indicator lights on the tool itself.  The Particle Electron should go from flashing green (connecting to cellular), flashing cyan (connecting to Internet) and breathing cyan (connected to Particle).  Indicators on the GPS module are red flashing every second (looking for satellites) and red flashing every 15 seconds (satellites acquired).  Once a data point is taken and the data is acknowledged by a one second flash on the blue D7 led on the Electron itself.
  2) In the Particle app, you can connect to this tool and see the number of data points collected as well as the signal strength.  
  3) By logging onto Ubidots, you can see the data points being recorded and the map pin moving on the dashboard.

# Additional Considerations

1) Test
