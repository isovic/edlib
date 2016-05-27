#include <unistd.h>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>
#include <ctime>
#include <string>
#include <climits>
#include <queue>
#include <sstream>

#include "edlib.h"

#include "SimpleEditDistance.h"

using namespace std;

int readFastaSequences(const char* path, vector< vector<unsigned char> >* seqs,
                       unsigned char* letterIdx, char* idxToLetter, bool* inAlphabet, int &alphabetLength);

void printAlignment(const unsigned char* query, const int queryLength,
                    const unsigned char* target, const int targetLength,
                    const unsigned char* alignment, const int alignmentLength,
                    const int position, const int modeCode, const char* idxToLetter);

void printSpectrum(const unsigned char* query, const int queryLength,
                    const unsigned char* target, const int targetLength,
                    const unsigned char* alignment, const int alignmentLength,
                    const int position, const int modeCode, const char* idxToLetter,
                    int kmer_size, FILE *fp_out);

// For debugging
void printSeq(const vector<unsigned char> &seq) {
    for (int i = 0; i < seq.size(); i++)
        printf("%d ", seq[i]);
    printf("\n");
}

int main(int argc, char * const argv[]) {
    
    //----------------------------- PARSE COMMAND LINE ------------------------//
    // If true, there will be no output.
    bool silent = false;
    // Alignment mode.
    char mode[16] = "NW";
    // How many best sequences (those with smallest score) do we want.
    // If 0, then we want them all.
    int numBestSeqs = 0;
    bool findAlignment = true;
    bool findStartLocations = true;
    int option;
    int kArg = -1;
    // If true, simple implementation of edit distance algorithm is used instead of edlib.
    // This is for testing purposes.
    bool useSimple = false;
    // If "STD" or "EXT", cigar string will be printed. if "NICE" nice representation
    // of alignment will be printed.
    char alignmentFormat[16] = "SPECT";
    int32_t kmer = 6;
    char out_spect_file[2048];
    sprintf (out_spect_file, "kmers.spect");

    bool invalidOption = false;
    while ((option = getopt(argc, argv, "m:n:k:f:spltj:o:")) >= 0) {
        switch (option) {
        case 'm': strcpy(mode, optarg); break;
        case 'n': numBestSeqs = atoi(optarg); break;
        case 'k': kArg = atoi(optarg); break;
        case 'f': strcpy(alignmentFormat, optarg); break;
        case 's': silent = true; break;
        case 'p': findAlignment = false; break;
        case 'l': findStartLocations = false; break;
        case 't': useSimple = true; break;
        case 'j': kmer = atoi(optarg); break;
        case 'o': strcpy(out_spect_file, optarg); break;
        default: invalidOption = true;
        }
    }
    if (optind + 2 != argc || invalidOption) {
        fprintf (stderr, "Tool for comparing the kmer spectrum of two sequences. For each position in the alignment, the kmers are output to stdout in a tab-delimited manner (for -f SPECT) option.\n");

        fprintf(stderr, "\n");
        fprintf(stderr, "Usage: aligner [options...] <queries.fasta> <target.fasta>\n");
        fprintf(stderr, "Options:\n");
        fprintf(stderr, "\t-s  If specified, there will be no score or alignment output (silent mode).\n");
        fprintf(stderr, "\t-m HW|NW|SHW  Alignment mode that will be used. [default: NW]\n");
        fprintf(stderr, "\t-n N  Score will be calculated only for N best sequences (best = with smallest score)."
                " If N = 0 then all sequences will be calculated."
                " Specifying small N can make total calculation much faster. [default: 0]\n");
        fprintf(stderr, "\t-k K  Sequences with score > K will be discarded."
                " Smaller k, faster calculation.\n");
        fprintf(stderr, "\t-t  If specified, simple algorithm is used instead of edlib. To be used for testing.\n");
        fprintf(stderr, "\t-p  If specified, alignment path will be found and printed. "
                "This may significantly slow down the calculation.\n");
        fprintf(stderr, "\t-l  If specified, start locations will be found and printed. "
                "Each start location corresponds to one end location. This may somewhat slow down "
                "the calculation, but is still faster then finding alignment path and does not consume "
                "any extra memory.\n");
        fprintf(stderr, "\t-f SPECT|NICE|CIG_STD|CIG_EXT  Format that will be used to print alignment path,"
                " can be used only with -p. SPECT will output the comparison of kmer-spectrum, NICE will give visually"
                " attractive format, CIG_STD will  give standard cigar format and CIG_EXT will give extended cigar format. [default: SPECT]\n");
        fprintf (stderr, "\t-j INT  The kmer size used for outputting the kmer spectrum. Used with -f SPECT option. [default: 6]\n");
        fprintf (stderr, "\t-o STR  Path to the file to output the kmer spectrum to.\n");
        return 1;
    }
    //-------------------------------------------------------------------------//

    if (strcmp(alignmentFormat, "SPECT") && strcmp(alignmentFormat, "NICE") && strcmp(alignmentFormat, "CIG_STD") &&
        strcmp(alignmentFormat, "CIG_EXT")) {
        printf("Invalid alignment path format (-f)!\n");
        return 1;
    }

    int modeCode;
    if (!strcmp(mode, "SHW"))
        modeCode = EDLIB_MODE_SHW;
    else if (!strcmp(mode, "HW"))
        modeCode = EDLIB_MODE_HW;
    else if (!strcmp(mode, "NW"))
        modeCode = EDLIB_MODE_NW;
    else {
        printf("Invalid mode (-m)!\n");
        return 1;
    }

    FILE *fp_out = fopen(out_spect_file, "w");
    fclose(fp_out);

    printf("Using %s alignment mode.\n", mode);


    // Alphabet information, will be constructed on fly while reading sequences
    unsigned char letterIdx[128]; //!< letterIdx[c] is index of letter c in alphabet
    char idxToLetter[128]; //!< numToLetter[i] is letter that has index i in alphabet
    bool inAlphabet[128]; // inAlphabet[c] is true if c is in alphabet
    for (int i = 0; i < 128; i++) {
        inAlphabet[i] = false;
    }
    int alphabetLength = 0;

    int readResult;
    // Read queries
    char* queriesFilepath = argv[optind];
    vector< vector<unsigned char> >* querySequences = new vector< vector<unsigned char> >();
    printf("Reading queries...\n");
    readResult = readFastaSequences(queriesFilepath, querySequences, letterIdx, idxToLetter,
                                    inAlphabet, alphabetLength);
    if (readResult) {
        printf("Error: There is no file with name %s\n", queriesFilepath);
        delete querySequences;
        return 1;
    }
    int numQueries = querySequences->size();
    int queriesTotalLength = 0;
    for (int i = 0; i < numQueries; i++) {
        queriesTotalLength += (*querySequences)[i].size();
    }
    printf("Read %d queries, %d residues total.\n", numQueries, queriesTotalLength);

    // Read target
    char* targetFilepath = argv[optind+1];    
    vector< vector<unsigned char> >* targetSequences = new vector< vector<unsigned char> >();
    printf("Reading target fasta file...\n");
    readResult = readFastaSequences(targetFilepath, targetSequences, letterIdx, idxToLetter,
                                    inAlphabet, alphabetLength);
    if (readResult) {
        printf("Error: There is no file with name %s\n", targetFilepath);
        delete querySequences;
        delete targetSequences;
        return 1;
    }
    unsigned char* target = (*targetSequences)[0].data();
    int targetLength = (*targetSequences)[0].size();
    printf("Read target, %d residues.\n", targetLength);

    printf("Alphabet: ");
    for (int c = 0; c < 128; c++)
        if (inAlphabet[c])
            printf("%c ", c);
    printf("\n");


    // ----------------------------- MAIN CALCULATION ----------------------------- //
    printf("\nComparing queries to target...\n");
    int* scores = new int[numQueries];
    int** endLocations = new int*[numQueries];
    int** startLocations = new int*[numQueries];
    int* numLocations = new int[numQueries];
    priority_queue<int> bestScores; // Contains numBestSeqs best scores
    int k = kArg;
    unsigned char* alignment = NULL; int alignmentLength;
    clock_t start = clock();

    if (!findAlignment || silent) {
        printf("0/%d", numQueries);
        fflush(stdout);
    }
    for (int i = 0; i < numQueries; i++) {
        unsigned char* query = (*querySequences)[i].data();
        int queryLength = (*querySequences)[i].size();
        // Calculate score
        if (useSimple) {
            // Just for testing
            calcEditDistanceSimple(query, queryLength, target, targetLength,
                                   alphabetLength, modeCode, scores + i,
                                   endLocations + i, numLocations + i);
        } else {
            edlibCalcEditDistance(query, queryLength, target, targetLength,
                                  alphabetLength, k, modeCode, findStartLocations, findAlignment,
                                  scores + i, endLocations + i, startLocations + i, numLocations + i,
                                  &alignment, &alignmentLength);
        }

        // If we want only numBestSeqs best sequences, update best scores 
        // and adjust k to largest score.
        if (numBestSeqs > 0) {
            if (scores[i] >= 0) {
                bestScores.push(scores[i]);
                if (bestScores.size() > numBestSeqs) {
                    bestScores.pop();
                }
                if (bestScores.size() == numBestSeqs) {
                    k = bestScores.top() - 1;
                    if (kArg >= 0 && kArg < k)
                        k = kArg;
                }
            }
        }
        
        if (!findAlignment || silent) {
            printf("\r%d/%d", i + 1, numQueries);
            fflush(stdout);
        } else {
            // Print alignment if it was found, use first position
            if (alignment) {
                printf("\n");
                printf("Query #%d (%d residues): score = %d\n", i, queryLength, scores[i]);
                if (!strcmp(alignmentFormat, "NICE")) {
                    printAlignment(query, queryLength, target, targetLength,
                                   alignment, alignmentLength,
                                   *(endLocations[i]), modeCode, idxToLetter);

                } else if (!strcmp(alignmentFormat, "SPECT")) {
                    FILE *fp_out = fopen(out_spect_file, "a");
                    fprintf (fp_out, ">Query #%d (%d residues): score = %d\n", i, queryLength, scores[i]);
                    printSpectrum(query, queryLength, target, targetLength,
                                   alignment, alignmentLength,
                                   *(endLocations[i]), modeCode, idxToLetter,
                                   kmer, fp_out);
                    fclose(fp_out);

                } else {
                    printf("Cigar:\n");
                    char* cigar = NULL;
                    int cigarFormat = !strcmp(alignmentFormat, "CIG_STD") ?
                        EDLIB_CIGAR_STANDARD : EDLIB_CIGAR_EXTENDED;
                    edlibAlignmentToCigar(alignment, alignmentLength, cigarFormat, &cigar);
                    if (cigar) {
                        printf("%s\n", cigar);
                        free(cigar);
                    } else {
                        printf("Error while printing cigar!\n");
                    }
                }
            }
        }

        if (alignment)
            free(alignment);
    }

    if (!silent && !findAlignment) {
        int scoreLimit = -1; // Only scores <= then scoreLimit will be printed (we consider -1 as infinity)
        printf("\n");

        if (bestScores.size() > 0) {
            printf("%d best scores:\n", (int)bestScores.size());
            scoreLimit = bestScores.top();
        } else {
            printf("Scores:\n");
        }

        printf("<query number>: <score>, <num_locations>, "
               "[(<start_location_in_target>, <end_location_in_target>)]\n");
        for (int i = 0; i < numQueries; i++) {
            if (scores[i] > -1 && (scoreLimit == -1 || scores[i] <= scoreLimit)) {
                printf("#%d: %d  %d", i, scores[i], numLocations[i]);
                if (numLocations[i] > 0) {
                    printf("  [");
                    for (int j = 0; j < numLocations[i]; j++) {
                        printf(" (");
                        if (startLocations[i]) {
                            printf("%d", *(startLocations[i] + j));
                        } else {
                            printf("?");
                        }
                        printf(", %d)", *(endLocations[i] + j));
                    }
                    printf(" ]");
                }
                printf("\n");
            }
        }
        
    }

    clock_t finish = clock();
    double cpuTime = ((double)(finish-start))/CLOCKS_PER_SEC;
    printf("\nCpu time of searching: %lf\n", cpuTime);
    // ---------------------------------------------------------------------------- //

    // Free allocated space
    for (int i = 0; i < numQueries; i++) {
        free(endLocations[i]);
        if (startLocations[i]) free(startLocations[i]);
    }
    delete[] endLocations;
    delete[] startLocations;
    delete[] numLocations;
    delete querySequences;
    delete targetSequences;
    delete[] scores;
    
    return 0;
}




/** Reads sequences from fasta file.
 * Function is passed current alphabet information and will update it if needed.
 * @param [in] path Path to fasta file containing sequences.
 * @param [out] seqs Sequences will be stored here, each sequence as vector of indexes from alphabet.
 * @param [inout] letterIdx  Array of length 128. letterIdx[c] is index of letter c in alphabet.
 * @param [inout] inAlphabet  Array of length 128. inAlphabet[c] is true if c is in alphabet.
 * @param [inout] alphabetLength
 * @return 0 if all ok, positive number otherwise.
 */
int readFastaSequences(const char* path, vector< vector<unsigned char> >* seqs,
                       unsigned char* letterIdx, char* idxToLetter, bool* inAlphabet, int &alphabetLength) {
    seqs->clear();
    
    FILE* file = fopen(path, "r");
    if (file == 0)
        return 1;

    bool inHeader = false;
    bool inSequence = false;
    int buffSize = 4096;
    char buffer[buffSize];
    while (!feof(file)) {
        int read = fread(buffer, sizeof(char), buffSize, file);
        for (int i = 0; i < read; ++i) {
            char c = buffer[i];
            if (inHeader) { // I do nothing if in header
                if (c == '\n')
                    inHeader = false;
            } else {
                if (c == '>') {
                    inHeader = true;
                    inSequence = false;
                } else {
                    if (c == '\r' || c == '\n')
                        continue;
                    // If starting new sequence, initialize it.
                    if (inSequence == false) {
                        inSequence = true;
                        seqs->push_back(vector<unsigned char>());
                    }

                    if (!inAlphabet[c]) { // I construct alphabet on fly
                        inAlphabet[c] = true;
                        letterIdx[c] = alphabetLength;
                        idxToLetter[alphabetLength] = c;
                        alphabetLength++;
                    }
                    seqs->back().push_back(letterIdx[c]);
                }
            }
        }
    }

    fclose(file);
    return 0;
}


void printAlignment(const unsigned char* query, const int queryLength,
                    const unsigned char* target, const int targetLength,
                    const unsigned char* alignment, const int alignmentLength,
                    const int position, const int modeCode, const char* idxToLetter) {
    int tIdx = -1;
    int qIdx = -1;
    if (modeCode == EDLIB_MODE_HW) {
        tIdx = position;
        for (int i = 0; i < alignmentLength; i++) {
            if (alignment[i] != 1)
                tIdx--;
        }
    }
    for (int start = 0; start < alignmentLength; start += 50) {
        // target
        printf("T: ");
        int startTIdx;
        for (int j = start; j < start + 50 && j < alignmentLength; j++) {
            if (alignment[j] == 1)
                printf("_");
            else
                printf("%c", idxToLetter[target[++tIdx]]);
            if (j == start)
                startTIdx = tIdx;
        }
        printf(" (%d - %d)\n", max(startTIdx, 0), tIdx);
        // query
        printf("Q: ");
        int startQIdx = qIdx;
        for (int j = start; j < start + 50 && j < alignmentLength; j++) {
            if (alignment[j] == 2)
                printf("_");
            else
                printf("%c", idxToLetter[query[++qIdx]]);
            if (j == start)
                startQIdx = qIdx;
        }
        printf(" (%d - %d)\n\n", max(startQIdx, 0), qIdx);
    }
}

void printSpectrum(const unsigned char* query, const int queryLength,
                    const unsigned char* target, const int targetLength,
                    const unsigned char* alignment, const int alignmentLength,
                    const int position, const int modeCode, const char* idxToLetter,
                    int kmer_size, FILE *fp_out) {
    int tIdx = -1;
    int qIdx = -1;
    if (modeCode == EDLIB_MODE_HW) {
        tIdx = position;
        for (int i = 0; i < alignmentLength; i++) {
            if (alignment[i] != 1)
                tIdx--;
        }
    }

    std::stringstream ss_ref;
    std::stringstream ss_query;
    for (int j = 0; j < alignmentLength; j++) {
        if (alignment[j] == 1)
            ss_ref << "-";
        else
            ss_ref << idxToLetter[target[++tIdx]];

        if (alignment[j] == 2)
            ss_query << "-";
        else
            ss_query << idxToLetter[query[++qIdx]];
    }

    std::string ref_str = ss_ref.str();
    std::string query_str = ss_query.str();

    for (int j = 0; j < (ref_str.size() - kmer_size + 1); j++) {
        fprintf (fp_out, "%d\t%s\t%s\n", j, ref_str.substr(j, kmer_size).c_str(), query_str.substr(j, kmer_size).c_str());
    }
}
