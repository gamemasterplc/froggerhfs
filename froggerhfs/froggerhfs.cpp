#define _CRT_SECURE_NO_WARNINGS //Shut up Visual Studio
#include <stdio.h>
#include <stdint.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif
#include <stdio.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#include "nlohmann/json.hpp"


//LZSS constants
#define N                4096   /* size of ring buffer */
#define F                  18   /* upper limit for match_length */
#define THRESHOLD       2   /* encode string into position and length
                                                   if match_length is greater than this */
#define NIL                     N       /* index for root of binary search trees */


#define HFS_SECTOR_SIZE 2048
#define HFS_NUM_SECTORS(size) (((size)+HFS_SECTOR_SIZE-1)/HFS_SECTOR_SIZE)

#define HFS_GET_SECTOR(file) ((file).ofs & 0xFFFFFF)
#define HFS_IS_COMPRESS(file) (((file).ofs >> 24) != 0)

struct HFSFileEntry {
    uint32_t ofs;
    uint32_t len;
};

struct HFSHeader {
    uint32_t magic;
    uint32_t size;
    uint32_t numFiles;
    uint32_t pad;
};

struct HFSFile {
    uint32_t id;
    bool compress;
    std::string name;
    std::vector<uint8_t> data;
    std::vector<uint8_t> dataCompressed;
};

std::vector<HFSFile> hfsFileList;

//LZSS variables
uint8_t text_buf[N + F - 1];    /* ring buffer of size N, with extra F-1 bytes to facilitate string comparison */
int match_position, match_length;  /* of longest match.  These are set by the InsertNode() procedure. */
int lson[N + 1], rson[N + 257], dad[N + 1];  /* left & right children & parents -- These constitute binary search trees. */

void to_json(nlohmann::json& j, const HFSFile& file)
{
    j = nlohmann::json{
        { "id", file.id },
        { "name", file.name },
        { "compress", file.compress }
    };
}

void from_json(const nlohmann::json& j, HFSFile& file)
{
    j.at("id").get_to(file.id);
    j.at("name").get_to(file.name);
    j.at("compress").get_to(file.compress);
}

uint32_t ReadMemoryBufU32(uint8_t* buf)
{
    //Convert 4 bytes into native endian 32-bit word
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

uint32_t ReadFileU32(FILE* file)
{
    uint8_t temp[4];
    if (fread(&temp[0], 4, 1, file) != 1) {
        std::cout << "Failed to read from file." << std::endl;
        exit(1);
    }
    return ReadMemoryBufU32(temp);
}

uint8_t ReadFileU8(FILE* file)
{
    uint8_t temp;
    if (fread(&temp, 1, 1, file) != 1) {
        std::cout << "Failed to read from file." << std::endl;
        exit(1);
    }
    return temp;
}

void WriteMemoryBufU32(uint8_t* buf, uint32_t value)
{
    //Split value into bytes in big-endian order
    buf[0] = value & 0xFF;
    buf[1] = (value >> 8) & 0xFF;
    buf[2] = (value >> 16) & 0xFF;
    buf[3] = value >> 24;
}

void WriteFileU32(FILE* file, uint32_t value)
{
    uint8_t temp[4];
    WriteMemoryBufU32(temp, value);
    //Write it
    fwrite(temp, 4, 1, file);
}

bool MakeDirectory(std::string dir)
{
    int ret;
#if defined(_WIN32)
    ret = _mkdir(dir.c_str());
#else 
    ret = mkdir(dir.c_str(), 0777); // notice that 777 is different than 0777
#endif]
    return ret != -1 || errno == EEXIST;
}

void ReadJSON(std::string in_file)
{
    std::ifstream file(in_file);
    if (!file.is_open()) {
        std::cout << "Failed to open " << in_file << " for reading." << std::endl;
        exit(1);
    }
    try {
        nlohmann::json json = nlohmann::json::parse(file);
        json.at("files").get_to(hfsFileList);
    }
    catch (nlohmann::json::exception& exception) {
        std::cout << exception.what() << std::endl;
        exit(1);
    }
}

void ReadFiles(std::string inName)
{
    for (size_t i = 0; i < hfsFileList.size(); i++) {
        std::string path = inName + "/" + hfsFileList[i].name;
        FILE* file = fopen(path.c_str(), "rb");
        if (!file) {
            std::cout << "Failed to open " << path << " for reading." << std::endl;
            exit(1);
        }
        fseek(file, 0, SEEK_END);
        size_t size = ftell(file);
        fseek(file, 0, SEEK_SET);
        hfsFileList[i].data.resize(size);
        fread(&hfsFileList[i].data[0], 1, size, file);
        fclose(file);
    }
}

void InitTree(void)  /* initialize trees */
{
    int  i;

    /* For i = 0 to N - 1, rson[i] and lson[i] will be the right and
       left children of node i.  These nodes need not be initialized.
       Also, dad[i] is the parent of node i.  These are initialized to
       NIL (= N), which stands for 'not used.'
       For i = 0 to 255, rson[N + i + 1] is the root of the tree
       for strings that begin with character i.  These are initialized
       to NIL.  Note there are 256 trees. */

    for (i = N + 1; i <= N + 256; i++) rson[i] = NIL;
    for (i = 0; i < N; i++) dad[i] = NIL;
}

void InsertNode(int r)
/* Inserts string of length F, text_buf[r..r+F-1], into one of the
   trees (text_buf[r]'th tree) and returns the longest-match position
   and length via the global variables match_position and match_length.
   If match_length = F, then removes the old node in favor of the new
   one, because the old one will be deleted sooner.
   Note r plays double role, as tree node and position in buffer. */
{
    int  i, p, cmp;
    uint8_t* key;

    cmp = 1;  key = &text_buf[r];  p = N + 1 + key[0];
    rson[r] = lson[r] = NIL;  match_length = 0;
    for (; ; ) {
        if (cmp >= 0) {
            if (rson[p] != NIL) p = rson[p];
            else { rson[p] = r;  dad[r] = p;  return; }
        }
        else {
            if (lson[p] != NIL) p = lson[p];
            else { lson[p] = r;  dad[r] = p;  return; }
        }
        for (i = 1; i < F; i++)
            if ((cmp = key[i] - text_buf[p + i]) != 0)  break;
        if (i > match_length) {
            match_position = p;
            if ((match_length = i) >= F)  break;
        }
    }
    dad[r] = dad[p];  lson[r] = lson[p];  rson[r] = rson[p];
    dad[lson[p]] = r;  dad[rson[p]] = r;
    if (rson[dad[p]] == p) rson[dad[p]] = r;
    else                   lson[dad[p]] = r;
    dad[p] = NIL;  /* remove p */
}

void DeleteNode(int p)  /* deletes node p from tree */
{
    int  q;

    if (dad[p] == NIL) return;  /* not in tree */
    if (rson[p] == NIL) q = lson[p];
    else if (lson[p] == NIL) q = rson[p];
    else {
        q = lson[p];
        if (rson[q] != NIL) {
            do { q = rson[q]; } while (rson[q] != NIL);
            rson[dad[q]] = lson[q];  dad[lson[q]] = dad[q];
            lson[q] = lson[p];  dad[lson[p]] = q;
        }
        rson[q] = rson[p];  dad[rson[p]] = q;
    }
    dad[q] = dad[p];
    if (rson[dad[p]] == p) rson[dad[p]] = q;  else lson[dad[p]] = q;
    dad[p] = NIL;
}

bool CompareFile(const HFSFile& a, const HFSFile& b)
{
    return a.id < b.id;
}

void CompressFile(HFSFile& file)
{
    int  i, c, len, r, s, last_match_length, code_buf_ptr;
    uint8_t code_buf[17], mask;
    size_t src_pos = 0;
    uint32_t codesize = 12;

    file.dataCompressed.resize(12);
    WriteMemoryBufU32(&file.dataCompressed[0], 0x31535250);
    WriteMemoryBufU32(&file.dataCompressed[4], file.data.size());
    WriteMemoryBufU32(&file.dataCompressed[8], 0);
    InitTree();  /* initialize trees */
    code_buf[0] = 0;  /* code_buf[1..16] saves eight units of code, and
            code_buf[0] works as eight flags, "1" representing that the unit
            is an unencoded letter (1 byte), "0" a position-and-length pair
            (2 bytes).  Thus, eight units require at most 16 bytes of code. */
    code_buf_ptr = mask = 1;
    s = 0;  r = N - F;
    for (i = s; i < r; i++) text_buf[i] = '\0';  /* Clear the buffer with
            any character that will appear often. */
    for (len = 0; len < F && src_pos < file.data.size(); len++)
        text_buf[r + len] = c = file.data[src_pos++];  /* Read F bytes into the last F bytes of
                the buffer */
    if (len == 0) return;  /* text of size zero */
    for (i = 1; i <= F; i++) InsertNode(r - i);  /* Insert the F strings,
            each of which begins with one or more 'space' characters.  Note
            the order in which these strings are inserted.  This way,
            degenerate trees will be less likely to occur. */
    InsertNode(r);  /* Finally, insert the whole string just read.  The
            global variables match_length and match_position are set. */
    do {
        if (match_length > len) match_length = len;  /* match_length
                may be spuriously long near the end of text. */
        if (match_length <= THRESHOLD) {
            match_length = 1;  /* Not long enough match.  Send one byte. */
            code_buf[0] |= mask;  /* 'send one byte' flag */
            code_buf[code_buf_ptr++] = text_buf[r];  /* Send uncoded. */
        }
        else {
            code_buf[code_buf_ptr++] = (uint8_t)match_position;
            code_buf[code_buf_ptr++] = (uint8_t)
                (((match_position >> 4) & 0xF0)
                    | (match_length - (THRESHOLD + 1)));  /* Send position and
                                  length pair. Note match_length > THRESHOLD. */
        }
        if ((mask <<= 1) == 0) {  /* Shift mask left one bit. */
            for (i = 0; i < code_buf_ptr; i++)  /* Send at most 8 units of */
                file.dataCompressed.push_back(code_buf[i]);    /* code together */
            codesize += code_buf_ptr;
            code_buf[0] = 0;  code_buf_ptr = mask = 1;
        }
        last_match_length = match_length;
        for (i = 0; i < last_match_length &&
            src_pos < file.data.size(); i++) {
            DeleteNode(s);          /* Delete old strings and */
            text_buf[s] = c = file.data[src_pos++];        /* read new bytes */
            if (s < F - 1) text_buf[s + N] = c;  /* If the position is
                    near the end of buffer, extend the buffer to make
                    string comparison easier. */
            s = (s + 1) & (N - 1);  r = (r + 1) & (N - 1);
            /* Since this is a ring buffer, increment the position
               modulo N. */
            InsertNode(r);  /* Register the string in text_buf[r..r+F-1] */
        }
        while (i++ < last_match_length) {       /* After the end of text, */
            DeleteNode(s);                                  /* no need to read, but */
            s = (s + 1) & (N - 1);  r = (r + 1) & (N - 1);
            if (--len) InsertNode(r);               /* buffer may not be empty. */
        }
    } while (len > 0);      /* until length of string to be processed is zero */
    if (code_buf_ptr > 1) {         /* Send remaining code. */
        for (i = 0; i < code_buf_ptr; i++) file.dataCompressed.push_back(code_buf[i]);
        codesize += code_buf_ptr;
    }
    WriteMemoryBufU32(&file.dataCompressed[8], codesize);

}
void CompressFiles()
{
    for (size_t i = 0; i < hfsFileList.size(); i++) {
        if (hfsFileList[i].compress) {
            CompressFile(hfsFileList[i]);
        }
        else {
            hfsFileList[i].dataCompressed = hfsFileList[i].data;
        }
    }
}

void WriteHFSHeader(FILE* file, const HFSHeader& header)
{
    WriteFileU32(file, header.magic);
    WriteFileU32(file, header.size);
    WriteFileU32(file, header.numFiles);
    WriteFileU32(file, header.pad);
}

void WriteFileEntry(FILE* file, const HFSFileEntry& fileEntry)
{
    WriteFileU32(file, fileEntry.ofs);
    WriteFileU32(file, fileEntry.len);
}

void AlignFileSector(FILE* file)
{
    uint32_t ofs = ftell(file);
    while (ofs % HFS_SECTOR_SIZE) {
        //Write zero until file is aligned
        uint8_t zero = 0;
        fwrite(&zero, 1, 1, file);
        ofs++;
    }
}

void WriteHFS(std::string outName)
{
    std::vector< HFSFileEntry> fileEntry;
    size_t sector = HFS_NUM_SECTORS((hfsFileList.size() * 8) + 16);

    for (size_t i = 0; i < hfsFileList.size(); i++) {
        HFSFileEntry entry;
        uint32_t len;
        entry.ofs = sector & 0xFFFFFF;
        if (hfsFileList[i].compress) {
            entry.ofs |= 0x01000000;
            len = hfsFileList[i].dataCompressed.size();
        }
        else {
            len = hfsFileList[i].data.size();
        }
        entry.len = len;
        sector += HFS_NUM_SECTORS(len);
        fileEntry.push_back(entry);

    }
    HFSHeader header;
    header.magic = 0x07736668;
    header.size = sector * HFS_SECTOR_SIZE;
    header.numFiles = hfsFileList.size();
    header.pad = 0;

    FILE* file = fopen(outName.c_str(), "wb");
    if (!file) {
        std::cout << "Failed to open " << outName << " for writing." << std::endl;
        exit(1);
    }

    WriteHFSHeader(file, header);
    for (size_t i = 0; i < fileEntry.size(); i++) {
        WriteFileEntry(file, fileEntry[i]);
    }
    AlignFileSector(file);
    for (size_t i = 0; i < hfsFileList.size(); i++) {
        if (hfsFileList[i].compress) {
            fwrite(&hfsFileList[i].dataCompressed[0], 1, hfsFileList[i].dataCompressed.size(), file);
        }
        else {
            fwrite(&hfsFileList[i].data[0], 1, hfsFileList[i].data.size(), file);
        }
        AlignFileSector(file);
    }
    fclose(file);
}

void PackHFS(std::string inName, std::string outName)
{
    ReadJSON(inName + "/" + "file.json");
    std::sort(hfsFileList.begin(), hfsFileList.end(), CompareFile);
    ReadFiles(inName);
    CompressFiles();
    WriteHFS(outName);
}

void ReadHFSHeader(FILE* file, HFSHeader& header)
{
    header.magic = ReadFileU32(file);
    header.size = ReadFileU32(file);
    header.numFiles = ReadFileU32(file);
    header.pad = ReadFileU32(file);
}

void ReadFileEntry(FILE* file, HFSFileEntry& fileEntry)
{
    fileEntry.ofs = ReadFileU32(file);
    fileEntry.len = ReadFileU32(file);
}

void DecodeLZSS(uint8_t* dst, uint8_t* src)
{
    uint32_t dst_pos = 0;
    size_t text_buf_pos = N - F;
    uint32_t flag = 0;
    uint32_t magic = ReadMemoryBufU32(&src[0]);
    uint32_t out_size = ReadMemoryBufU32(&src[4]);
    uint32_t in_size = ReadMemoryBufU32(&src[8]);
    if (magic != 0x31535250) { //PRS1 in big endian
        std::cout << "Invalid LZSS data." << std::endl;
        exit(1);
    }
    src += 12;
    in_size -= 12;
    memset(text_buf, 0, N + F - 1);
    while (dst_pos < out_size) {
        if (!(flag & 0x100)) {
            uint8_t value = *src++;
            flag = 0xFF00 | value;
        }
        if (flag & 0x1) {
            uint8_t value = *src++;
            text_buf[text_buf_pos] = dst[dst_pos++] = value;
            text_buf_pos = (text_buf_pos + 1) % N;
        }
        else {
            uint8_t byte1 = *src++;
            uint8_t byte2 = *src++;
            size_t ofs = ((byte2 & 0xF0) << 4) | byte1;
            size_t copy_size = (byte2 & 0xF) + THRESHOLD + 1;
            for (size_t i = 0; i < copy_size; i++) {
                dst[dst_pos++] = text_buf[text_buf_pos] = text_buf[ofs];
                ofs = (ofs + 1) % N;
                text_buf_pos = (text_buf_pos + 1) % N;
            }
        }
        flag >>= 1;
    }
}

std::string GetHFSFileExtenson(HFSFile& file)
{
    //Very small sizes are assumed to be .bin
    if (file.data.size() < 16) {
        return ".bin";
    }
    uint32_t sectionMagic = ReadMemoryBufU32(&file.data[0]);
    if (sectionMagic == 0x24) {
        //If first section is model TOC, then its a DFF or BSP
        //TODO: Add code to distinguish BSP from DFF
        return ".dff";
    }
    if (sectionMagic == 0x1B) {
        //If first section is Animation Informaton its renderware animation file
        return ".anm";
    }
    //Do offset-based check for sprite files
    uint32_t layerOfs = ReadMemoryBufU32(&file.data[12]);
    if (layerOfs == 0x14) {
        return ".atb";
    }
    //Unknown file type
    return ".bin";
}

void ReadHFSFile(FILE* hfsFile, uint32_t id, HFSFileEntry& fileEntry, HFSFile &file)
{
    file.id = id;
    
    file.dataCompressed.resize(fileEntry.len);
    fseek(hfsFile, HFS_GET_SECTOR(fileEntry) * HFS_SECTOR_SIZE, SEEK_SET);
    fread(&file.dataCompressed[0], 1, fileEntry.len, hfsFile);
    file.compress = HFS_IS_COMPRESS(fileEntry);
    if (HFS_IS_COMPRESS(fileEntry)) {
        size_t rawSize = ReadMemoryBufU32(&file.dataCompressed[4]);
        file.data.resize(rawSize);
        DecodeLZSS(&file.data[0], &file.dataCompressed[0]);
    }
    else {
        file.data = file.dataCompressed;
    }
    file.name = std::to_string(id) + GetHFSFileExtenson(file);
}

void ReadHFS(std::string inName)
{
    FILE* hfsFile = fopen(inName.c_str(), "rb");
    HFSHeader header;
    ReadHFSHeader(hfsFile, header);
    fseek(hfsFile, 16, SEEK_SET);
    std::vector<HFSFileEntry> entryList;
    for (uint32_t i = 0; i < header.numFiles; i++) {
        HFSFileEntry entry;
        ReadFileEntry(hfsFile, entry);
        entryList.push_back(entry);
    }
    for (uint32_t i = 0; i < header.numFiles; i++) {
        HFSFile file;
        ReadHFSFile(hfsFile, i, entryList[i], file);
        hfsFileList.push_back(file);
    }
    fclose(hfsFile);
}

void DumpHFS(std::string outName)
{
    std::string outDir = outName + "/";
    if (!MakeDirectory(outDir)) {
        std::cout << "Failed to create " << outDir << "." << std::endl;
        exit(1);
    }
    std::string jsonPath = outName + "/" + "file.json";
    std::ofstream jsonFile(jsonPath);
    nlohmann::json json;
    if (!jsonFile.is_open()) {
        std::cout << "Failed to open " << jsonPath << " for writing." << std::endl;
        exit(1);
    }
    json["files"] = hfsFileList;
    jsonFile << json.dump(4);
    for (uint32_t i = 0; i < hfsFileList.size(); i++) {
        std::string fileName = outDir + hfsFileList[i].name;
        FILE* file = fopen(fileName.c_str(), "wb");
        if (!file) {
            std::cout << "Failed to open " << fileName << " for writing." << std::endl;
            exit(1);
        }
        fwrite(&hfsFileList[i].data[0], hfsFileList[i].data.size(), 1, file);
        fclose(file);
    }
}
void UnpackHFS(std::string inName, std::string outName)
{
    ReadHFS(inName);
    DumpHFS(outName);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) {
        std::cout << "Usage: " << argv[0] << " p/u input output" << std::endl;
        std::cout << "p is used in the second argument when packing the input directory into an output HFS file." << std::endl;
        std::cout << "u is used in the second argument when dumping an input HFS file into a base path." << std::endl;
        std::cout << "The output parameter is optional and will generate an output name based on the input name if not provided." << std::endl;
        return 1;
    }
    std::string optionArg = argv[1];
    std::string inName = argv[2];
    std::string outName;
    if (argc == 4) {
        outName = argv[3];
    }
    else {
        outName = inName.substr(0, inName.find_last_of("."));
    }
    if (optionArg == "p") {
        if (argc != 4) {
            outName += ".hfs";
        }
        PackHFS(inName, outName);
    }
    else if (optionArg == "u") {
        UnpackHFS(inName, outName);
    }
    else {
        std::cout << "Invalid option argument " << optionArg << std::endl;
        return 1;
    }

    return 0;
}