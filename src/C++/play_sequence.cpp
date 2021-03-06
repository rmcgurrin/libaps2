#include <iostream>
#include <iterator>
#include <algorithm>
#include <signal.h>

#include "libaps2.h"
#include "concol.h"
#include "helpers.h"

#include "optionparser.h"

enum  optionIndex { UNKNOWN, HELP, SEQ_FILE, TRIG_MODE, TRIG_INTERVAL, LOG_LEVEL};
const option::Descriptor usage[] =
{
	{UNKNOWN, 0,"" , ""    , option::Arg::None, "USAGE: play_sequence [options]\n\n"
	                                         "Options:" },
	{HELP,    0,"" , "help", option::Arg::None, "  --help  \tPrint usage and exit." },
	{SEQ_FILE, 0,"", "seqFile", option::Arg::Required, "  --seqFile  \tHDF5 sequence file to play." },
	{TRIG_MODE, 0,"", "trigMode", option::Arg::Required, "  --trigMode  \tTrigger mode (0: external; 1: internal; 2: software - optional; default=1)." },
	{TRIG_INTERVAL,  0,"", "trigInterval", option::Arg::Numeric, "  --trigRep  \t(optional) Internal trigger interval (optional; default=10ms)." },
	{LOG_LEVEL,  0,"", "logLevel", option::Arg::Numeric, "  --logLevel  \t(optional) Logging level level to print to console (optional; default=2/INFO)." },
	{UNKNOWN, 0,"" ,  ""   , option::Arg::None, "\nExamples:\n"
	                                         "  play_sequence --seqFile=../examples/ramsey_tppi.h5\n"
	                                         "  play_sequence --seqFile=../examples/rampey_slipped.h5 --trigMode=2\n" },
	{0,0,0,0,0,0}
};


int main(int argc, char* argv[])
{

	print_title("BBN APS2 Sequence Player");

	argc-=(argc>0); argv+=(argc>0); // skip program name argv[0] if present
	option::Stats  stats(usage, argc, argv);
	option::Option *options = new option::Option[stats.options_max];
	option::Option *buffer = new option::Option[stats.buffer_max];
	option::Parser parse(usage, argc, argv, options, buffer);

	if (parse.error())
	 return -1;

	if (options[HELP] || argc == 0) {
		option::printUsage(std::cout, usage);
		return 0;
	}

	for (option::Option* opt = options[UNKNOWN]; opt; opt = opt->next())
	 std::cout << "Unknown option: " << opt->name << "\n";

	for (int i = 0; i < parse.nonOptionsCount(); ++i)
	 std::cout << "Non-option #" << i << ": " << parse.nonOption(i) << "\n";

	//Logging level
	TLogLevel logLevel = logINFO;
	if (options[LOG_LEVEL]) {
		logLevel = TLogLevel(atoi(options[LOG_LEVEL].arg));
	}

	//Trigger source -- default of internal
	TRIGGER_SOURCE triggerSource = INTERNAL;
	if (options[TRIG_MODE]) {
		triggerSource = TRIGGER_SOURCE(atoi(options[TRIG_MODE].arg));
	}
	set_logging_level(logLevel);
	set_log("stdout");

	//Trigger interval -- default of 10ms
	double trigInterval = 10e-3;
	if (options[TRIG_INTERVAL]) {
		trigInterval = atof(options[TRIG_INTERVAL].arg);
	}

	string seqFile;
	if (options[SEQ_FILE]){
		seqFile = string(options[SEQ_FILE].arg);
	} else {
		std::cerr << "A sequence file is required.";
		return -1;
	}

	string deviceSerial = get_device_id();
	if (deviceSerial.empty()){
		cout << concol::RED << "No APS2 devices connected! Exiting..." << concol::RESET << endl;
		return 0;
	}

	connect_APS(deviceSerial.c_str());

	double uptime;
	get_uptime(deviceSerial.c_str(), &uptime);

	cout << concol::CYAN << "Uptime for device " << deviceSerial << " is " << uptime << " seconds" << concol::RESET << endl;

	// force initialize device
	init_APS(deviceSerial.c_str(), 1);

	//load the sequence file
	load_sequence_file(deviceSerial.c_str(), seqFile.c_str());

	//Set the trigger mode
	set_trigger_source(deviceSerial.c_str(), triggerSource);

	//Trigger interval
	set_trigger_interval(deviceSerial.c_str(), trigInterval);

	//Set to sequence mode
	set_run_mode(deviceSerial.c_str(), RUN_SEQUENCE);

	run(deviceSerial.c_str());

	//For software trigger, trigger on key stroke
	if (triggerSource == 2) {
		cout << concol::YELLOW << "Press t-Return to trigger or q-Return to exit" << concol::RESET << endl;
		while(true) {
			char keyStroke = cin.get();
			if (keyStroke == 't') {
				trigger(deviceSerial.c_str());
			} else if (keyStroke == 'q') {
				break;
			}
		}
	}
	else {
		cout << concol::YELLOW << "Press any key to stop" << concol::RESET;
		cin.get();
	}


	stop(deviceSerial.c_str());
	disconnect_APS(deviceSerial.c_str());

	delete[] options;
	delete[] buffer;
	return 0;
 }