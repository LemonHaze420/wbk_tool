#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <algorithm>

struct ImaAdpcmState {
    int valprev = 0;
    int index = 0;
};
const int stepsizeTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18499, 20350, 22385, 24623, 27086, 29794, 32767
};
const int indexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

// taken from ALSA
std::vector<int16_t> DecodeImaAdpcm(const std::vector<uint8_t>& samples, int num_samples)
{
    std::vector<int16_t> outBuff;
    ImaAdpcmState state;
    
    for (auto code : samples) {
        short pred_diff;	/* Predicted difference to next sample */
        short step;		/* holds previous StepSize value */
        char sign;
        /* Separate sign and magnitude */
        sign = code & 0x8;
        code &= 0x7;
        /*
         * Computes pred_diff = (code + 0.5) * step / 4,
         * but see comment in adpcm_coder.
         */
        step = stepsizeTable[state.index];
        /* Compute difference and new predicted value */
        pred_diff = step >> 3;
        for (int i = 0x4; i; i >>= 1, step >>= 1) {
            if (code & i) {
                pred_diff += step;
            }
        }
        state.valprev += (sign) ? -pred_diff : pred_diff;
        /* Clamp output value */
        if (state.valprev > 32767) {
            state.valprev = 32767;
        }
        else if (state.valprev < -32768) {
            state.valprev = -32768;
        }
        /* Find new StepSize index value */
        state.index += indexTable[code];
        if (state.index < 0) {
            state.index = 0;
        }
        else if (state.index > 88) {
            state.index = 88;
        }
        int16_t val = static_cast<int16_t>(state.valprev);
        outBuff.push_back(val);
    }
    return outBuff;
}
class WBK {
public:
    struct header_t {
        char magic[8];
        char unk[8];
        int flag;
        int size;
        int sample_data_offs;
        int total_bytes;
        char name[32];
        int num_entries;
        int val5, val6, val7;
        int offs;
        int metadata_offs;
        int offs3;
        int offs4;
        int num;
        int entry_desc_offs;
    } header;

    struct metadata_t {
        char codec;
        char flags[3];
        uint32_t unk_vals;
        float unk_fvals[6];
    };
    struct nslWave
    {
        int hash;
        unsigned char codec;
        char field_5;
        unsigned char flags;
        char field_7;
        int num_samples;
        unsigned int num_bytes;
        int field_10;
        int field_14;
        int field_18;
        int compressed_data_offs;
        unsigned __int16 samples_per_second;
        __int16 field_22;
        int unk;
    };

    std::vector <nslWave> entries;
    std::vector<std::vector<int16_t>> tracks;
    std::vector<metadata_t> metadata;

    static int GetNumChannels(const nslWave& wave) {
        int num_channels = 0;
        if (wave.flags)
            num_channels = (((((wave.flags & 0x55) + ((wave.flags >> 1) & 0x55)) & 0x33)
                + ((((wave.flags & 0x55) + ((wave.flags >> 1) & 0x55)) >> 2) & 0x33)) & 0xF)
            + (((((wave.flags & 0x55) + ((wave.flags >> 1) & 0x55)) & 0x33)
                + ((((wave.flags & 0x55) + ((wave.flags >> 1) & 0x55)) >> 2) & 0x33)) >> 4);
        else
            num_channels = 1;
        return num_channels;
    }

    static int GetNumSamples(const nslWave& wave)
    {
        unsigned int tmp_flag = (unsigned __int8)((((wave.flags & 0x55) + ((wave.flags >> 1) & 0x55)) & 0x33)     +
                                (((unsigned __int8)((wave.flags & 0x55) + ((wave.flags >> 1) & 0x55)) >> 2) & 0x33));
        if (wave.codec == 1)
        {
            if (wave.flags)
                return ((tmp_flag & 0xF) + (tmp_flag >> 4)) * wave.num_bytes;
            else
                return wave.num_bytes;
        }
        else if (wave.codec == 2)
        {
            int bytes = 0;
            if (wave.flags)
                bytes = ((tmp_flag & 0xF) + (tmp_flag >> 4)) * wave.num_bytes;
            else
                bytes = wave.num_bytes;
            return 2 * bytes;
        }
        else
            return wave.num_samples;
    }

    void read(std::string path)
    {
        std::ifstream stream(path, std::ios::binary);
        if (stream.good()) {
            stream.seekg(0, std::ios::end);
            size_t actual_file_size = stream.tellg();
            stream.seekg(0, std::ios::beg);
            stream.read(reinterpret_cast<char*>(&header), sizeof header_t);
            printf("Bank Name: %s\n", std::string(header.name).c_str());

            // read all entries

            for (int index = 0; index < header.num_entries; ++index) {
                nslWave entry;
                stream.seekg(0x100 + (sizeof nslWave * index), std::ios::beg);
                stream.read(reinterpret_cast<char*>(&entry), sizeof nslWave);

                // calc bits per sample & blockAlign
                int bits_per_sample = 0;
                int size = 0;
                int blockAlign = 0;
                if (entry.codec == 1 || entry.codec == 2) {
                    bits_per_sample = 8 * (entry.codec != 1) + 8;
                    blockAlign = (GetNumChannels(entry) * bits_per_sample) / 8;
                }
                else if (entry.codec == 4) {
                    bits_per_sample = 16;
                    blockAlign = (bits_per_sample * GetNumChannels(entry)) / 8;
                }
                else if (entry.codec == 5) {
                    bits_per_sample = 4;
                    blockAlign = 36 * GetNumChannels(entry);
                }
                else if (entry.codec == 7) {
                    bits_per_sample = 16;
                    blockAlign = (bits_per_sample * GetNumChannels(entry)) / 8;
                }

                // calc size depending on format
                int fmt_type = entry.codec - 4;
                if (fmt_type) {
                    int tmp_type = fmt_type - 1;
                    if (!tmp_type)
                        size = blockAlign * (entry.num_bytes >> 6);
                    else if (tmp_type != 2)
                        size = entry.num_bytes;
                    else
                        size = 4 * entry.num_samples;
                }
                else
                    size = 2 * entry.num_bytes;


                printf("Hash: 0x%08X codec=%d num_samples=%d num_channels=%d rate=%dHz bps=%d length=%fs\n", entry.hash, entry.codec, GetNumSamples(entry), GetNumChannels(entry), entry.samples_per_second, bits_per_sample, (float)GetNumSamples(entry) / entry.samples_per_second);
                
                // PCM(?)
                if (entry.codec == 1 || entry.codec == 2) {
                    stream.seekg(0x1000, std::ios::beg);
                    std::vector<int16_t> tmp;
                    for (int i = 0; i < size / 4; ++i) {
                        int16_t sample1, sample2;
                        stream.read(reinterpret_cast<char*>(&sample1), 2);
                        stream.read(reinterpret_cast<char*>(&sample2), 2);
                        tmp.push_back(sample1);
                        tmp.push_back(sample2);
                    }
                    tracks.push_back(tmp);
                }
                // IMA ADPCM
                else if (entry.codec == 7)
                {
                    std::vector<uint8_t> bdata;
                    stream.seekg(entry.compressed_data_offs, std::ios::beg);
                    auto samples_size = entry.num_bytes;
                    if (entry.compressed_data_offs + size > actual_file_size)
                        samples_size = actual_file_size - entry.compressed_data_offs;
                    bdata.resize(size);
                    stream.read((char*)bdata.data(), samples_size);
                    tracks.push_back(DecodeImaAdpcm(bdata, GetNumSamples(entry)));
                }
                // @todo: codecs 5 (BINK) and 4 remaining
                else
                {
                    printf("Unsupported codec (%d)!\n", entry.codec);
                }
                entries.push_back(entry);
            }

            size_t num_metadata = (header.entry_desc_offs - header.metadata_offs) / sizeof metadata_t;
            stream.seekg(header.metadata_offs, std::ios::beg);
            for (int index = 0; index < num_metadata; ++index) {
                metadata_t tmp_metadata;
                stream.read(reinterpret_cast<char*>(&tmp_metadata), sizeof metadata_t);
                metadata.push_back(tmp_metadata);
                printf("metadata #%d\tcodec = 0x%x\t", index+1, tmp_metadata.codec);
                for (int i = 0; i < 6; ++i)
                    printf("%f%s", tmp_metadata.unk_fvals[i], i != 5 ? ", " : "\n");
            }
            char desc[16] = { '\0' };
            stream.read(reinterpret_cast<char*>(&desc), 16);
            printf("Bank Type: %s\n", std::string(desc).c_str());

            stream.close();
        }
    }
};

static void writeWAV(const std::string& filename, const std::vector<int16_t>& samples, uint32_t sampleRate, int nchannels = 1) {
    struct WAVHeader {
        char riff[4] = { 'R', 'I', 'F', 'F' };
        uint32_t chunkSize;
        char wave[4] = { 'W', 'A', 'V', 'E' };
        char fmt[4] = { 'f', 'm', 't', ' ' };
        uint32_t subchunk1Size = 16;
        uint16_t audioFormat = 1;
        uint16_t numChannels = 1;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample = 16;
        char data[4] = { 'd', 'a', 't', 'a' };
        uint32_t subchunk2Size;
    } header;
    header.sampleRate = sampleRate;
    header.numChannels = nchannels;
    header.bitsPerSample = 16;
    header.blockAlign = header.numChannels * (header.bitsPerSample / 8);
    header.byteRate = header.sampleRate * header.blockAlign;
    header.subchunk2Size = samples.size() * sizeof(int16_t);
    header.chunkSize = 36 + header.subchunk2Size;

    std::ofstream outFile(filename, std::ios::binary);
    if (!outFile) {
        std::cerr << "Failed to open file for writing: " << filename << std::endl;
        return;
    }
    outFile.write(reinterpret_cast<const char*>(&header), sizeof(WAVHeader));
    outFile.write(reinterpret_cast<const char*>(samples.data()), samples.size() * sizeof(int16_t));
    outFile.close();

    std::cout << "WAV file written: " << filename << std::endl;
}

int main(int argc, char** argv)
{
    printf("WBK Tool - LemonHaze 2025\n");
    if (argc != 3) {
        printf("Usage: %s <.wbk> <.wav>\n", argv[0]);
        return -1;
    }

    WBK wbk;
    wbk.read(argv[1]);
    
    size_t index = 0;
    for (auto& track : wbk.tracks) {
        WBK::nslWave& entry = wbk.entries[index];
        std::filesystem::path output_path = std::string(argv[2]).append(std::to_string(index+1)).append(".wav");
        writeWAV(output_path.string(), track, entry.samples_per_second / WBK::GetNumChannels(entry), WBK::GetNumChannels(entry));
        ++index;
    }
    return 1;
}