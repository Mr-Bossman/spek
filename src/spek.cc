#include <pthread.h>

#include <wx/cmdline.h>
#include <wx/log.h>
#include <wx/socket.h>

#include "spek-artwork.h"
#include "spek-audio.h"
#include "spek-fft.h"
#include "spek-pipeline.h"
#include "spek-platform.h"
#include "spek-preferences.h"

#include "spek-window.h"

struct FreqRangeData {
    float *peak_levels = nullptr;
    int bands = 0;
    bool done = false;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
};

static void freq_range_cb(int bands, int sample, float *values, void *cb_data)
{
    auto *data = static_cast<FreqRangeData *>(cb_data);
    if (sample == -1) {
        pthread_mutex_lock(&data->mutex);
        data->done = true;
        pthread_cond_signal(&data->cond);
        pthread_mutex_unlock(&data->mutex);
        return;
    }
    if (!data->peak_levels) {
        data->bands = bands;
        data->peak_levels = new float[bands];
        for (int i = 0; i < bands; i++) {
            data->peak_levels[i] = -200.0f;
        }
    }
    for (int i = 0; i < bands; i++) {
        if (values[i] > data->peak_levels[i]) {
            data->peak_levels[i] = values[i];
        }
    }
}

class Spek: public wxApp
{
public:
    Spek() : wxApp(), window(NULL), quit(false) {}

protected:
    virtual bool OnInit();
    virtual int OnRun();
#ifdef OS_OSX
    virtual void MacOpenFiles(const wxArrayString& files);
#endif

private:
    SpekWindow *window;
    wxString path;
    bool quit;
};

IMPLEMENT_APP(Spek)

bool Spek::OnInit()
{
    wxInitAllImageHandlers();
    wxSocketBase::Initialize();

    spek_artwork_init();
    spek_platform_init();
    SpekPreferences::get().init();

    static const wxCmdLineEntryDesc desc[] = {{
            wxCMD_LINE_SWITCH,
            "h",
            "help",
            "Show this help message",
            wxCMD_LINE_VAL_NONE,
            wxCMD_LINE_OPTION_HELP,
        }, {
            wxCMD_LINE_SWITCH,
            "V",
            "version",
            "Display the version and exit",
            wxCMD_LINE_VAL_NONE,
            wxCMD_LINE_PARAM_OPTIONAL,
        }, {
            wxCMD_LINE_SWITCH,
            "r",
            "freq-range",
            "Print the min and max frequency and exit",
            wxCMD_LINE_VAL_NONE,
            wxCMD_LINE_PARAM_OPTIONAL,
        }, {
            wxCMD_LINE_PARAM,
            NULL,
            NULL,
            "FILE",
            wxCMD_LINE_VAL_STRING,
            wxCMD_LINE_PARAM_OPTIONAL,
        },
        wxCMD_LINE_DESC_END,
    };

    wxCmdLineParser parser(desc, argc, argv);
    int ret = parser.Parse(true);
    if (ret == 1) {
        return false;
    }
    if (ret == -1) {
        this->quit = true;
        return true;
    }
    if (parser.Found("version")) {
        // TRANSLATORS: the %s is the package version.
        wxPrintf(_("Spek version %s"), PACKAGE_VERSION);
        wxPrintf("\n");
        this->quit = true;
        return true;
    }
    if (parser.Found("freq-range")) {
        if (!parser.GetParamCount()) {
            wxPrintf("Error: FILE argument required\n");
            this->quit = true;
            return false;
        }
        std::string file_name(parser.GetParam().utf8_str());

        Audio audio;
        FFT fft;
        int fft_bits = 11;
        int nfft = 1 << fft_bits;
        auto file = audio.open(file_name, 0);
        if (file->get_error() != AudioError::OK) {
            wxPrintf("Error: cannot open input file\n");
            this->quit = true;
            return true;
        }

        int sample_rate = file->get_sample_rate();

        FreqRangeData data;
        pthread_mutex_init(&data.mutex, NULL);
        pthread_cond_init(&data.cond, NULL);

        auto pipeline = spek_pipeline_open(
            std::move(file),
            fft.create(fft_bits),
            0, 0,
            WINDOW_DEFAULT,
            1024,
            freq_range_cb,
            &data
        );
        spek_pipeline_start(pipeline);

        pthread_mutex_lock(&data.mutex);
        while (!data.done) {
            pthread_cond_wait(&data.cond, &data.mutex);
        }
        pthread_mutex_unlock(&data.mutex);

        float threshold = -120.0f;
        int min_bin = -1, max_bin = -1;
        if (data.peak_levels) {
            for (int i = 1; i < data.bands; i++) {
                if (data.peak_levels[i] > threshold) {
                    if (min_bin == -1) min_bin = i;
                    max_bin = i;
                }
            }
        }

        if (min_bin >= 0) {
            double min_freq = (double)min_bin * sample_rate / nfft;
            double max_freq = (double)max_bin * sample_rate / nfft;
            wxPrintf("Min frequency: %.0f Hz\n", min_freq);
            wxPrintf("Max frequency: %.0f Hz\n", max_freq);
        } else {
            wxPrintf("No frequencies above %.0f dB found\n", threshold);
        }

        delete[] data.peak_levels;
        pthread_mutex_destroy(&data.mutex);
        pthread_cond_destroy(&data.cond);
        spek_pipeline_close(pipeline);

        this->quit = true;
        return true;
    }

    if (parser.GetParamCount()) {
        this->path = parser.GetParam();
    }

    this->window = new SpekWindow(this->path);
    this->window->Show(true);
    SetTopWindow(this->window);
    return true;
}

int Spek::OnRun()
{
    if (quit) {
        return 0;
    }

    return wxApp::OnRun();
}

#ifdef OS_OSX
void Spek::MacOpenFiles(const wxArrayString& files)
{
    if (files.GetCount() == 1) {
        this->window->open(files[0]);
    }
}
#endif
