#include "common.h"
#include "FIFORequestChannel.h"

#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace std;

namespace {

// Format helper that matches the BIMDC CSV style by trimming trailing zeros.
string format_three_decimal(double value) {
    ostringstream oss;
    oss << fixed << setprecision(3) << value;
    string out = oss.str();
    if (out.find('.') != string::npos) {
        while (!out.empty() && out.back() == '0') {
            out.pop_back();
        }
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }
    if (out.empty()) {
        out = "0";
    }
    return out;
}

// Produces a stable textual form for ECG values without scientific notation.
string format_signal_value(double value) {
    ostringstream oss;
    oss << defaultfloat << setprecision(6) << value;
    string out = oss.str();
    if (out.find('.') != string::npos) {
        while (!out.empty() && out.back() == '0') {
            out.pop_back();
        }
        if (!out.empty() && out.back() == '.') {
            out.pop_back();
        }
    }
    if (out.empty()) {
        out = "0";
    }
    return out;
}

void ensure_directory_exists(const string& path) {
    if (mkdir(path.c_str(), 0777) == -1) {
        if (errno == EEXIST) {
            return;
        }
        perror("mkdir");
        throw runtime_error("failed to create directory: " + path);
    }
}

void send_quit(FIFORequestChannel& chan) {
    MESSAGE_TYPE quit = QUIT_MSG;
    int written = chan.cwrite(&quit, sizeof(MESSAGE_TYPE));
    if (written != static_cast<int>(sizeof(MESSAGE_TYPE))) {
        throw runtime_error("failed to send QUIT message");
    }
}

double request_datapoint(FIFORequestChannel& chan, int person, double seconds, int ecgno) {
    datamsg msg(person, seconds, ecgno);
    int written = chan.cwrite(&msg, sizeof(datamsg));
    if (written != static_cast<int>(sizeof(datamsg))) {
        throw runtime_error("failed to write datamsg");
    }
    double response = 0.0;
    int received = chan.cread(&response, sizeof(double));
    if (received != static_cast<int>(sizeof(double))) {
        throw runtime_error("failed to read datapoint response");
    }
    return response;
}

void dump_first_1000(FIFORequestChannel& chan, int person, const string& out_csv) {
    ofstream output(out_csv, ios::trunc);
    if (!output.is_open()) {
        throw runtime_error("failed to open output file: " + out_csv);
    }

    for (int i = 0; i < 1000; ++i) {
        double timestamp = i * 0.004;
        double ecg1 = request_datapoint(chan, person, timestamp, 1);
        double ecg2 = request_datapoint(chan, person, timestamp, 2);

        output << format_three_decimal(timestamp) << ','
               << format_signal_value(ecg1) << ','
               << format_signal_value(ecg2);
        if (i != 999) {
            output << '\n';
        }
    }
}

__int64_t request_filesize(FIFORequestChannel& chan, const string& fname) {
    filemsg header(0, 0);
    size_t total = sizeof(filemsg) + fname.size() + 1;
    vector<char> request(total);
    memcpy(request.data(), &header, sizeof(filemsg));
    strcpy(request.data() + sizeof(filemsg), fname.c_str());

    int written = chan.cwrite(request.data(), static_cast<int>(total));
    if (written != static_cast<int>(total)) {
        throw runtime_error("failed to write file size request");
    }

    __int64_t response = 0;
    int received = chan.cread(&response, sizeof(__int64_t));
    if (received != static_cast<int>(sizeof(__int64_t))) {
        throw runtime_error("failed to read file size response");
    }
    return response;
}

void fetch_file(FIFORequestChannel& chan, const string& fname, size_t buffercap) {
    if (fname.empty()) {
        throw runtime_error("no filename specified for download");
    }
    if (buffercap == 0) {
        throw runtime_error("buffer capacity must be positive");
    }
    if (buffercap > static_cast<size_t>(numeric_limits<int>::max())) {
        throw runtime_error("buffer capacity too large for protocol");
    }

    __int64_t filesize = request_filesize(chan, fname);
    if (filesize < 0) {
        throw runtime_error("server returned negative file size");
    }

    ensure_directory_exists("received");
    string outpath = "received/" + fname;

    int fd = open(outpath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        throw runtime_error("failed to open output file: " + outpath);
    }

    filemsg header(0, 0);
    size_t request_size = sizeof(filemsg) + fname.size() + 1;
    vector<char> request(request_size);
    vector<char> buffer(buffercap);

    auto start = chrono::steady_clock::now();

    __int64_t offset = 0;
    while (offset < filesize) {
        int chunk = static_cast<int>(min(static_cast<__int64_t>(buffercap), filesize - offset));
        header.offset = offset;
        header.length = chunk;
        memcpy(request.data(), &header, sizeof(filemsg));
        strcpy(request.data() + sizeof(filemsg), fname.c_str());

        int written = chan.cwrite(request.data(), static_cast<int>(request_size));
        if (written != static_cast<int>(request_size)) {
            close(fd);
            throw runtime_error("failed to write file chunk request");
        }

        int received = chan.cread(buffer.data(), chunk);
        if (received != chunk) {
            close(fd);
            throw runtime_error("failed to read expected file chunk");
        }

        ssize_t stored = pwrite(fd, buffer.data(), received, offset);
        if (stored != received) {
            close(fd);
            throw runtime_error("failed to write file chunk to disk");
        }
        offset += received;
    }

    auto end = chrono::steady_clock::now();
    close(fd);

    auto elapsed = chrono::duration_cast<chrono::milliseconds>(end - start).count();
    cout << "transfer=" << filesize
         << " buffercap=" << buffercap
         << " elapsed_ms=" << elapsed << endl;
}

unique_ptr<FIFORequestChannel> request_new_channel(FIFORequestChannel& control) {
    MESSAGE_TYPE request = NEWCHANNEL_MSG;
    int written = control.cwrite(&request, sizeof(MESSAGE_TYPE));
    if (written != static_cast<int>(sizeof(MESSAGE_TYPE))) {
        throw runtime_error("failed to request new channel");
    }

    array<char, MAX_MESSAGE> name_buffer{};
    int received = control.cread(name_buffer.data(), name_buffer.size());
    if (received <= 0) {
        throw runtime_error("failed to read new channel name");
    }

    string channel_name(name_buffer.data());
    return make_unique<FIFORequestChannel>(channel_name, FIFORequestChannel::CLIENT_SIDE);
}

} // namespace

int main(int argc, char* argv[]) {
    int option = 0;
    bool patient_specified = false;
    bool time_specified = false;
    bool ecg_specified = false;
    bool file_specified = false;
    bool new_channel_requested = false;
    bool buffercap_specified = false;

    int patient = 0;
    double seconds = 0.0;
    int ecg = 0;
    string filename;
    size_t buffercap = MAX_MESSAGE;

    while ((option = getopt(argc, argv, "p:t:e:f:m:c")) != -1) {
        switch (option) {
            case 'p':
                patient = atoi(optarg);
                patient_specified = true;
                break;
            case 't':
                seconds = atof(optarg);
                time_specified = true;
                break;
            case 'e':
                ecg = atoi(optarg);
                ecg_specified = true;
                break;
            case 'f':
                filename = optarg;
                file_specified = true;
                break;
            case 'm': {
                errno = 0;
                char* endptr = nullptr;
                long value = strtol(optarg, &endptr, 10);
                if (errno != 0 || endptr == optarg || value <= 0) {
                    cerr << "invalid buffer capacity" << endl;
                    return 1;
                }
                buffercap = static_cast<size_t>(value);
                buffercap_specified = true;
                break;
            }
            case 'c':
                new_channel_requested = true;
                break;
            default:
                cerr << "usage error" << endl;
                return 1;
        }
    }

    if (time_specified != ecg_specified) {
        cerr << "-t and -e must be provided together" << endl;
        return 1;
    }
    if ((time_specified || ecg_specified) && !patient_specified) {
        cerr << "-p is required with -t and -e" << endl;
        return 1;
    }
    if (!patient_specified && !file_specified) {
        cerr << "no operation requested" << endl;
        return 1;
    }

    pid_t child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        return 1;
    }

    if (child_pid == 0) {
        string capacity_string;
        vector<char*> args;
        args.push_back(const_cast<char*>("./server"));
        if (buffercap_specified) {
            capacity_string = to_string(buffercap);
            args.push_back(const_cast<char*>("-m"));
            args.push_back(const_cast<char*>(capacity_string.c_str()));
        }
        args.push_back(nullptr);
        if (execvp("./server", args.data()) == -1) {
            perror("execvp");
            _exit(1);
        }
    }

    unique_ptr<FIFORequestChannel> control_channel;
    unique_ptr<FIFORequestChannel> new_channel;
    int exit_code = 0;

    try {
        control_channel = make_unique<FIFORequestChannel>("control", FIFORequestChannel::CLIENT_SIDE);
        FIFORequestChannel* control = control_channel.get();
        FIFORequestChannel* active = control;

        if (new_channel_requested) {
            new_channel = request_new_channel(*control);
            active = new_channel.get();
        }

        bool do_single = patient_specified && time_specified && ecg_specified;
        bool do_dump = patient_specified && !(time_specified && ecg_specified);

        if (do_single) {
            double value = request_datapoint(*active, patient, seconds, ecg);
            cout << defaultfloat << setprecision(15) << value << endl;
        }

        if (do_dump) {
            dump_first_1000(*active, patient, "x1.csv");
        }

        if (file_specified) {
            fetch_file(*active, filename, buffercap);
        }

        if (new_channel) {
            send_quit(*new_channel);
        }
        if (control_channel) {
            send_quit(*control_channel);
        }
    } catch (const exception& ex) {
        cerr << "client error: " << ex.what() << endl;
        exit_code = 1;
        if (new_channel) {
            try {
                send_quit(*new_channel);
            } catch (...) {
            }
        }
        if (control_channel) {
            try {
                send_quit(*control_channel);
            } catch (...) {
            }
        }
    }

    int status = 0;
    if (waitpid(child_pid, &status, 0) == -1) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 1;
    }
    return exit_code;
}

