/*  junctions_creator.h -- Declarations for `junctions create` command

    Copyright (c) 2015, The Griffith Lab

    Author: Avinash Ramu <aramu@genome.wustl.edu>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.  */

#include <getopt.h>
#include <iostream>
#include <sstream>
#include "junctions_creator.h"
#include "sam.h"
#include "faidx.h"
#include "kstring.h"

using namespace std;

//Parse the options passed to this tool
int JunctionsCreator::parse_options(int argc, char *argv[]) {
    optind = 1; //Reset before parsing again.
    int c;
    while((c = getopt(argc, argv, "ha:o:")) != -1) {
        switch(c) {
            case 'a':
                min_anchor_length = atoi(optarg);
                break;
            case 'o':
                output_file = string(optarg);
                break;
            case '?':
            case 'h':
            default:
                return 1;
        }
    }
    if(argc == optind) {
        cerr << endl << "Error parsing inputs!" << endl;
        return 1;
    }
    bam_ = string(argv[optind]);
    cerr << endl << "Minimum junction anchor length: " << min_anchor_length;
    cerr << endl << "BAM file: " << bam_;
    return 0;
}

//Usage statement for this tool
int JunctionsCreator::usage(ostream& out) {
    out << "\nUsage:\t\t" << "regtools junctions create [options] alignments.bam";
    out << "\n";
    return 0;
}

//Get the BAM filename
string JunctionsCreator::get_bam() {
    return bam_;
}

//Add a junction to the junctions map
//The read_count field is the number of reads supporting the junction.
int JunctionsCreator::add_junction(Junction j1) {
    cerr << endl << "Adding junction\n";
    //cerr << "\nChr " << j1.chrom << "\tStart " << j1.start << "\tEnd " << j1.end;
    stringstream s1;
    string start, end;
    s1 << j1.start; start = s1.str();
    s1 << j1.end; end = s1.str();
    string key = j1.chrom + string(":") + start + "-" + end;
    if((j1.start - j1.thick_start >= min_anchor_length) &&
           (j1.thick_end - j1.end >= min_anchor_length)) {
        j1.has_min_anchor = true;
    }
    //Check if new junction
    if(!junctions.count(key)) {
        j1.read_count = 1;
    } else { //existing junction
        Junction j0 = junctions[key];
        //increment read count
        j1.read_count = j0.read_count + 1;
        //Check if thick starts are any better
        if(j0.thick_start < j1.thick_start)
            j1.thick_start = j0.thick_start;
        if(j0.thick_end > j1.thick_end)
            j1.thick_end = j0.thick_end;
        //preserve min anchor information
        if(j0.has_min_anchor)
            j1.has_min_anchor = true;
        if(j1.strand != j0.strand) {
            print_one_junction(j1);
            throw "Strand information doesn't match.";
        }

    }
    junctions[key] = j1;
    return 0;
}

//Print one junction
void JunctionsCreator::print_one_junction(const Junction j1, ostream& out) {
    out << j1.chrom <<
        "\t" << j1.thick_start << "\t" << j1.thick_end <<
        "\t" << j1.start << "\t" << j1.end <<
        "\t" << j1.strand << "\t" << j1.read_count << endl;
}

//Print all the junctions - this function needs work
void JunctionsCreator::print_all_junctions(ostream& out) {
    ofstream fout;
    if(!output_file.empty())
        fout.open(output_file.c_str());
    for(map<string, Junction> :: iterator it = junctions.begin();
        it != junctions.end(); it++) {
        Junction j1 = it->second;
        if (j1.has_min_anchor)
            if(fout.is_open())
                print_one_junction(j1, fout);
            else
                print_one_junction(j1, out);
    }
    if(fout.is_open())
        fout.close();
}

//Check if the junction has the minimum anchor length
bool JunctionsCreator::properly_anchored(Junction j1) {
    return((j1.start - j1.thick_start >= min_anchor_length) &&
           (j1.thick_end - j1.end >= min_anchor_length));
}

//Get the strand from the XS aux tag
void JunctionsCreator::set_junction_strand(bam1_t *aln, Junction& j1) {
    uint8_t *p = bam_aux_get(aln, "XS");
    if(p != NULL) {
        char strand = bam_aux2A(p);
        strand ? j1.strand = string(1, strand) : j1.strand = string(1, '?');
    } else {
        j1.strand = string(1, '?');
        return;
    }
}

//Parse junctions from the read and store in junction map
int JunctionsCreator::parse_alignment_into_junctions(bam_hdr_t *header, bam1_t *aln) {
    const bam1_core_t *c = &aln->core;
    if (c->n_cigar <= 1) // max one cigar operation exists(likely all matches)
        return 0;

    int chr_id = aln->core.tid;
    int read_pos = aln->core.pos;
    string chr(header->target_name[chr_id]);
    uint32_t *cigar = bam_get_cigar(aln);
    int n_cigar = c->n_cigar;

    Junction j1;
    j1.chrom = chr;
    j1.start = read_pos; //maintain start pos of junction
    j1.thick_start = read_pos;
    set_junction_strand(aln, j1);
    bool started_junction = false;
    cerr << "\nread_pos " << read_pos;
    for (int i = 0; i < n_cigar; ++i) {
        char op =
               bam_cigar_opchr(cigar[i]);
        int len =
               bam_cigar_oplen(cigar[i]);
        cerr << "\ncigar " << op << " " << len;
        switch(op) {
            //Add first junction if read overlaps
            // two junctions
            case 'N':
                if(!started_junction) {
                    j1.end = j1.start + len;
                    j1.thick_end = j1.end;
                    started_junction = true;
                } else {
                    cerr << endl << "DEBUG " << read_pos << "\t" <<
                        j1.start << "\t" << j1.end << "\t" <<
                        j1.thick_start << "\t" << j1.thick_end << endl;
                    //Add the previous junction
                    add_junction(j1);
                    j1.added = true;
                    //Start the next junction
                    j1.added = false;
                    j1.thick_start = j1.end;
                    j1.start = j1.thick_end;
                    j1.end = j1.start + len;
                    j1.thick_end = j1.end;
                    started_junction = true;
                }
                break;
            case 'D':
            case '=':
            case 'X':
            case 'M':
                if(!started_junction)
                    j1.start += len;
                else
                    j1.thick_end += len;
                break;
            //SEQ not in reference genome - skip
            case 'I':
            case 'S':
            case 'H':
                break;
            default:
                cerr << "Unknown cigar " << op;
                break;
        }
    }
    if(!j1.added) {
        cerr << endl << "DEBUG2 " << read_pos << "\t" <<
            j1.start << "\t" << j1.end << "\t" << j1.thick_start << "\t" << j1.thick_end << endl;
        add_junction(j1);
        j1.added = true;
    return 0;
}

//Pull out the cigar string from the read
int JunctionsCreator::parse_read(bam_hdr_t *header, bam1_t *aln) {
    const bam1_core_t *c = &aln->core;
    if (c->n_cigar) { // cigar
        int chr_id = aln->core.tid;
        int read_pos = aln->core.pos;
        string chr(header->target_name[chr_id]);
        uint32_t *cigar = bam_get_cigar(aln);
        int n_cigar = c->n_cigar;
        parse_cigar_into_junctions(chr, read_pos, cigar, n_cigar);
    }
}

//The workhorse - identifies junctions from BAM
int JunctionsCreator::identify_junctions_from_BAM() {
    if(!bam_.empty()) {
        cerr << endl << "Opening BAM " << bam_ << endl;
        //open BAM for reading
        samFile *in = sam_open(bam_.c_str(), "r");
        if(in == NULL) {
            return 1;
        }
        //Get the header
        bam_hdr_t *header = sam_hdr_read(in);
        if(header == NULL) {
            sam_close(in);
            return 1;
        }
        //Initiate the alignment record
        bam1_t *aln = bam_init1();
        while(sam_read1(in, header, aln) >= 0) {
            parse_alignment_into_junctions(header, aln);
        }
        bam_destroy1(aln);
        bam_hdr_destroy(header);
        sam_close(in);
    }
    return 0;
}
