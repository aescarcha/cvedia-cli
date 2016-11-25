#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <vector>
#include <algorithm>
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <deque>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <assert.h>
#include <math.h>
#include <sys/types.h>
#include <unistd.h>
#include <curl/curl.h>
#include <sys/ioctl.h>

#include "rapidjson/document.h"
#include "rapidjson/writer.h"
#include "rapidjson/stringbuffer.h"

#include "api.hpp"
#include "functions.hpp"
#include "optionparser.h"
#include "cvedia.hpp"
#include "curlreader.hpp"
#include "csvwriter.hpp"
#include "caffeimagedata.hpp"

using namespace std;
using namespace rapidjson;

// Initialize global variables
int gDebug 		= 1;

int gBatchSize 	= 256;
int gDownloadThreads = 100;

int lTermWidth = 80;

string gBaseDir = "";
string gExportName = "";
string gApiUrl = "http://api.cvedia.com/";
string gOutputFormat = "csv";
string gJobID = "";

string gVersion = CLI_VERSION;
string gAPIVersion = API_VERSION;

enum  optionIndex { UNKNOWN, HELP, JOB, DIR, NAME, API, OUTPUT, BATCHSIZE, THREADS, MOD_CSV_SAME_DIR};
const option::Descriptor usage[] =
{
	{UNKNOWN, 	0,"" , ""    ,option::Arg::None, string("CVEDIA-CLI v." + gVersion + " API Compatibility v." + gAPIVersion + "\n\nUSAGE: cvedia [options]\n\nOptions:").c_str() },
	{HELP,    	0,"" , "help",option::Arg::None, "  --help  \tPrint usage and exit." },
	{JOB,    	0,"j", "job",option::Arg::Required, "  --job=<id>, -j <id>  \tAPI Job ID" },
	{DIR,    	0,"d", "dir",option::Arg::Required, "  --dir=<path>, -d <path>  \tBase path for storing exported data (default: .)" },
	{NAME,    	0,"n", "name",option::Arg::Required, "  --name=<arg>, -n <arg>  \tName used for storing data on disk (defaults to jobid)" },
	{OUTPUT,   	0,"o", "output",option::Arg::Required, "  --output=<module>, -o <module>  \tSupported modules are CSV, CaffeImageData. (default: csv)" },
	{BATCHSIZE, 0,"b", "batch-size",option::Arg::Required, "  --batch-size=<num>, -b <num>  \tNumber of images to retrieve in a single batch (default: 256)." },
	{THREADS,   0,"t", "threads",option::Arg::Required, "  --threads=<num>, -t <num>  \tNumber of download threads (default: 100)." },
	{API,    	0,"", "api",option::Arg::Required, "  --api=<url>  \tREST API Connecting point (default: http://api.cvedia.com/)"  },
	{UNKNOWN, 	0,"" ,  ""   ,option::Arg::None, "\nExamples:\n\tcvedia -j d41d8cd98f00b204e9800998ecf8427e\n\tcvedia -j d41d8cd98f00b204e9800998ecf8427e -n test_export --api=http://api.cvedia.com/\n" },
	{0,0,0,0,0,0}
};

int main(int argc, char* argv[]) {
	struct winsize w;
	
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
	lTermWidth = w.ws_col;
	
	argc-=(argc>0); argv+=(argc>0); // skip program name argv[0] if present
	option::Stats  stats(usage, argc, argv);
	option::Option options[stats.options_max], buffer[stats.buffer_max];
	option::Parser parse(usage, argc, argv, options, buffer);
	
	if (parse.error())
		return 1;
	
	if (options[HELP] || argc == 0 || !options[JOB]) {
		option::printUsage(std::cout, usage);
		return 0;
	}

	if (options[BATCHSIZE].count() == 1) {
		gBatchSize = atoi(options[BATCHSIZE].arg);
	}

	if (options[DIR].count() == 1) {
		gBaseDir = options[DIR].arg;
	}

	if (options[JOB].count() == 1) {
		gJobID = options[JOB].arg;
	}
	
	if (options[NAME].count() == 1) {
		gExportName = options[NAME].arg;
	} else { // fallback to jobid for name
		gExportName = gJobID;
	}
	
	if (options[API].count() == 1) {
		gApiUrl = options[API].arg;
	}
	
	if (options[THREADS].count() == 1) {
		gDownloadThreads = atoi(options[THREADS].arg);
	}
	
	if (options[OUTPUT].count() == 1) {
		gOutputFormat = options[OUTPUT].arg;
	
		// Convert to lowercase
		std::transform(gOutputFormat.begin(), gOutputFormat.end(), gOutputFormat.begin(), ::tolower);
	}

	// Initialize the Curl library
	// We could've done this inside the curlreader but with those being created
	// and destroyed continually this is a better approach
	CURLcode res = curl_global_init(CURL_GLOBAL_NOTHING);
	if (res != 0) {
		cout << "curl_global_init(): " << curl_easy_strerror(res) << endl;
	}
	
	if (InitializeApi() == 0) {
		StartExport("");
	}
	
	return 1;
}

int StartExport(string export_code) {

	time_t seconds;

	map<string,string> options;
	options["base_dir"] = gBaseDir;
	options["create_test_file"] = "1";
	options["create_train_file"] = "1";
	options["create_validate_file"] = "1";

	CurlReader *p_reader = new CurlReader();

	IDataWriter *p_writer = NULL;

	if (gOutputFormat == "csv") {
		p_writer = new CsvWriter(gExportName, options);
	} else if (gOutputFormat == "caffeimagedata") {
		p_writer = new CaffeImageDataWriter(gExportName, options);
	} else {
		WriteErrorLog(string("Unsupported output module specified: " + gOutputFormat).c_str());
	}

	if (p_writer->Initialize() != 0) {
		WriteErrorLog("Failed to initialize CsvWriter");
		return -1;
	}

	p_reader->SetNumThreads(gDownloadThreads);

	int batch_size = GetTotalDatasetSize(export_code);
	WriteDebugLog(string("Total expected dataset size is " + to_string(batch_size)).c_str());

	// Fetch basic stats on export
	int num_batches = ceil(batch_size / gBatchSize);

	for (int batch_idx = 0; batch_idx < num_batches; batch_idx++) {

		vector<Metadata* > meta_data = FetchBatch(export_code, batch_idx);

		if (meta_data.size() == 0) {
			WriteDebugLog(string("No metadata return by API, end of dataset?").c_str());
			return -1;
		}

		WriteDebugLog(string("Starting download for batch #" + to_string(batch_idx)).c_str());

		seconds = time(NULL);

		// Queue up all requests in this batch
		for (Metadata* m : meta_data) {
			p_reader->QueueUrl(m->filename, m->url);
		}

		ReaderStats stats = p_reader->GetStats();

		// Loop until all downloads are finished
		while (stats.num_reads_completed < gBatchSize) {

			stats = p_reader->GetStats();

			if (time(NULL) != seconds) {
				DisplayProgressBar(stats.num_reads_completed / (float)gBatchSize, stats.num_reads_completed, gBatchSize);

//				cout << batch_idx << " " << stats.num_reads_success << " " << stats.num_reads_empty << " " << stats.num_reads_error << endl;

				seconds = time(NULL);
			}

			usleep(100);
		}

		// Display the 100% complete
		DisplayProgressBar(1, stats.num_reads_completed, gBatchSize);
		cout << endl;

		// Update stats for last time
		stats = p_reader->GetStats();

		WriteDebugLog(string("Downloaded " + to_string(stats.bytes_read) + " bytes").c_str());
		WriteDebugLog("Syncing batch to disk...");

		p_reader->ClearStats();

		map<string, ReadRequest* > responses = p_reader->GetAllData();

		for (Metadata* m : meta_data) {
			// Find the request for a specific filename
			ReadRequest* req = responses[m->filename];

			if (req != NULL && req->read_data.size() > 0) {
				// We found the download for a piece of metadata
				m->image_data = req->read_data;

				p_writer->WriteData(m);
			}
		}

		p_reader->ClearData();

		// We are completely done with the response data
		for (auto& kv : responses) {
			delete kv.second;
		}

		// Flush any other data from memory to disk
		p_writer->Finalize();

		responses.clear();
	}

	return 0;
}

void DisplayProgressBar(float progress, int cur_value, int max_value) {
	cout << "[";
	int bar_width = lTermWidth - (to_string(cur_value).length() + to_string(max_value).length() + 6);
	int pos = bar_width * progress;
	
	for (int i = 0; i < bar_width; ++i) {
		if (i < pos) cout << "=";
		else if (i == pos) cout << ">";
		else cout << " ";
	}
	
	cout << "] [" << to_string(cur_value) << "/" << to_string(max_value) << "]\r";
	cout.flush();
}
