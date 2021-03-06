#include <iostream>

#include "headings.h"
#include "libaps2.h"
#include "constants.h"

#include <concol.h>

using namespace std;

int get_device_id() {
  cout << "Choose device ID [0]: ";
  string input = "";
  getline(cin, input);

  if (input.length() == 0) {
    return 0;
  }
  int device_id;
  stringstream mystream(input);

  mystream >> device_id;
  return device_id;
}

int main (int argc, char* argv[])
{

  concol::concolinit();
  cout << concol::RED << "BBN AP2 Test Executable" << concol::RESET << endl;


  //Logging level
  TLogLevel logLevel = logDEBUG1;
  // if (options[LOG_LEVEL]) {
  //   logLevel = TLogLevel(atoi(options[LOG_LEVEL].arg));
  // }
  set_logging_level(logLevel);
  set_log("stdout");

  cout << concol::RED << "Enumerating devices" << concol::RESET << endl;

  unsigned numDevices;
  get_numDevices(&numDevices);

  cout << concol::RED << numDevices << " APS device" << (numDevices > 1 ? "s": "")  << " found" << concol::RESET << endl;

  if (numDevices < 1)
  	return 0;

  cout << concol::RED << "Attempting to get serials" << concol::RESET << endl;

  const char ** serialBuffer = new const char*[numDevices];
  get_deviceSerials(serialBuffer);

  for (unsigned cnt=0; cnt < numDevices; cnt++) {
  	cout << concol::RED << "Device " << cnt << " serial #: " << serialBuffer[cnt] << concol::RESET << endl;
  }

  string deviceSerial;

  if (numDevices == 1) {
    deviceSerial = string(serialBuffer[0]);
  } else {
    deviceSerial = string(serialBuffer[get_device_id()]);
  }

  cout << concol::RED << "Connecting to device serial #: " << deviceSerial << concol::RESET << endl;

  connect_APS(deviceSerial.c_str());

  reset(deviceSerial.c_str(), static_cast<int>(APS_RESET_MODE_STAT::RECONFIG_EPROM));

  disconnect_APS(deviceSerial.c_str());

  delete[] serialBuffer;

  cout << concol::RED << "Finished!" << concol::RESET << endl;

  return 0;
}
