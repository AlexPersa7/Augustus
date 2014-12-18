/**********************************************************************
 * file:    geneMSA.cc
 * licence: Artistic Licence, see file LICENCE.TXT or 
 *          http://www.opensource.org/licenses/artistic-license.php
 * descr.:  Generation of exon candidates
 * author:  Alexander Gebauer
 *
 * date    |   author           |  changes
 * --------|--------------------|------------------------------------------
 * 04.04.12| Alexander Gebauer  | creation of the file
 * 02.08.13| Mario Stanke       | rewrite of the merging of alignment blocks
 * 09.08.13| Mario Stanke       | rewrite of most (createOrthoExons, getCodonAlignment and others)
 **********************************************************************/

#include "geneticcode.hh"
#include "genomicMSA.hh"
#include "geneMSA.hh"
#include "intronmodel.hh"
#include "namgene.hh"
#include "orthograph.hh"
#include <fstream>
#include <iostream>
#include <string>
#include <sys/time.h>
#include <unordered_set>

using namespace std;

PhyloTree *GeneMSA::tree = NULL;
CodonEvo *GeneMSA::codonevo = NULL;
int GeneMSA::padding = 1000; // added to seqRange after last alignment block
int GeneMSA::orthoExonID = 1;
int GeneMSA::geneRangeID = 1;
vector<int> GeneMSA::exonCandID;
vector<ofstream*> GeneMSA::exonCands_outfiles;
vector<ofstream*> GeneMSA::orthoExons_outfiles;
vector<ofstream*> GeneMSA::geneRanges_outfiles;
vector<ofstream*> GeneMSA::omega_outfiles;
unordered_map< bit_vector, PhyloTree*> GeneMSA::topologies;

/*
 * constructor of GeneMSA
 */
GeneMSA::GeneMSA(RandSeqAccess *rsa, Alignment *a) {
    int maxDNAPieceSize = Properties::getIntProperty( "maxDNAPieceSize" );

    this->rsa = rsa;
    alignment = a;
    if (!alignment)
	throw ProjectError("Internal error in GeneMSA: Alignment missing.");    
    if (alignment->numRows() != rsa->getNumSpecies())
	throw ProjectError("Error in GeneMSA: Number of species in alignment (" + itoa(alignment->numRows())
			   + ") is not matching the one in the tree (" + itoa(rsa->getNumSpecies()) + ").");
    ltree = NULL; // locus/gene tree, may be different from species tree
    exoncands.resize(alignment->numRows(), NULL);
    /** construct the gene ranges
     * now: simple copy. TODO: extend region when apparently part of the alignment is missing
     * human   ***********---*******************
     * mouse   *******-------******************-
     * chicken ***********----------------------
     *                        ^
     *                        | extend range here
     */
    starts.resize(alignment->numRows(), -1);
    ends.resize(alignment->numRows(), -1);
    offsets.resize(alignment->numRows(), 0);
    for (size_t s=0; s < alignment->numRows(); s++){
	if (alignment->rows[s]){
	    int chrLen = rsa->getChrLen(s, alignment->rows[s]->seqID);
	    switch (getStrand(s))
		{
		case plusstrand:
		    starts[s] = alignment->rows[s]->chrStart() - padding;
		    ends[s] = alignment->rows[s]->chrEnd() + padding;
		    break;
		case minusstrand:
		    starts[s] = chrLen - alignment->rows[s]->chrEnd() - padding;
		    ends[s] = chrLen - alignment->rows[s]->chrStart() + padding;
		    break;
		default:
		    throw ProjectError("GeneMSA: Unknown strand in alignment.");
		}
	    // ensure that gene predictions on each region will be done IN ONE PIECE
	    int geneRangeLen = ends[s]-starts[s]+1;
	    if (geneRangeLen > maxDNAPieceSize + 2*padding){
		cerr << "Warning: length of gene range(" << geneRangeLen << ") is species " << rsa->getSname(s) <<  " exceeds maxDNAPieceSize(" 
		     << maxDNAPieceSize << "). Will shorten sequence at both ends to achieve a length of " << maxDNAPieceSize + 2*padding << endl;
		int tooMuch = geneRangeLen - (maxDNAPieceSize + 2*padding);
		starts[s] += (tooMuch + 1)/2;
		ends[s] -= (tooMuch + 1)/2;
		// delete all fragments that do not overlap with the gene range  
                AlignmentRow *row = alignment->rows[s];
                vector<fragment>::iterator first = row->frags.begin();
                while (first != row->frags.end() && first->chrPos + first->len - 1 < starts[s]){
                    ++first;
                }
                row->frags.erase(row->frags.begin(),first);
                vector<fragment>::iterator last = row->frags.begin();
                while (last != row->frags.end() && last->chrPos < ends[s]){
                    ++last;
                }
                row->frags.erase(last,row->frags.end());
                // shorten fragments that are not fully included in the gene range    
                if(!row->frags.empty()){
                    if(row->chrStart() < starts[s]){
                        int diff= starts[s] - row->chrStart();
                        row->frags[0].chrPos += diff;
                        row->frags[0].aliPos += diff;
                        row->frags[0].len -= diff;
                    }
                    if(row->chrEnd() > ends[s]){
                        int diff= row->chrEnd() - ends[s];
                        row->frags[row->frags.size()-1].len -= diff;
		    }
		}
		//TODO: update cumFragLen (probably no longer needed)
	    }
	    if (starts[s] < 0)
		starts[s] = 0;
	    if (ends[s] < 0)
		ends[s] = 0;
	    if (ends[s] >= chrLen)
		ends[s] = chrLen-1;
	    if (starts[s] >= chrLen)
		starts[s] = chrLen-1;
	    offsets[s] = (getStrand(s) == plusstrand)? starts[s] : rsa->getChrLen(s, getSeqID(s)) - 1 - ends[s];
	}
    }
}

/*
 * destructor of GeneMSA
 */
GeneMSA::~GeneMSA(){
    if (alignment)
	delete alignment;
    for (int i=0; i<exoncands.size(); i++) {
	if (exoncands[i]!=NULL) {
	    for(list<ExonCandidate*>::iterator it = exoncands[i]->begin(); it != exoncands[i]->end(); it++){
		delete *it;
	    }
	    exoncands.at(i)->clear();
	    delete exoncands[i];
	}
    }
}

string GeneMSA::getSeqID(int speciesIdx) {
    if (alignment->rows[speciesIdx])
	return alignment->rows[speciesIdx]->seqID;
    else
	return "";
}

Strand GeneMSA::getStrand(int speciesIdx){
    if (alignment->rows[speciesIdx])
	return alignment->rows[speciesIdx]->strand;
    else 
	return STRAND_UNKNOWN;
}

// adds the keys to the map function
map<string,ExonCandidate*>* GeneMSA::getECHash(list<ExonCandidate*> *ec) {
    map<string, ExonCandidate*> *hashCandidates = new  map<string, ExonCandidate*>;
    if (ec->empty())
	return NULL;
	    
    for (list<ExonCandidate*>::iterator lit=ec->begin(); lit!=ec->end(); lit++)
	(*hashCandidates)[(*lit)->key()] = *lit;

    return hashCandidates;
}

// computes and sets the exon candidates for species s
// and inserts them into the hash of ECs if they do not exist already
void GeneMSA::createExonCands(int s, const char *dna, map<int_fast64_t, ExonCandidate*> &ecs, map<int_fast64_t, ExonCandidate*> &addECs){
    double assmotifqthresh = 0.15;
    double assqthresh = 0.3;
    double dssqthresh = 0.7;
    bool onlySampledECs = false;
    int minEClen = 1;
    Properties::assignProperty("/CompPred/onlySampledECs", onlySampledECs);
    if(!onlySampledECs){
	Properties::assignProperty("/CompPred/assmotifqthresh", assmotifqthresh);
	Properties::assignProperty("/CompPred/assqthresh", assqthresh);
	Properties::assignProperty("/CompPred/dssqthresh", dssqthresh);
	// TODO Properties::assignProperty("/CompPred/minExonCandLen", minEClen);
    
	findExonCands(ecs,addECs, dna, minEClen, assmotifqthresh, assqthresh, dssqthresh); 
    }
    list<ExonCandidate*> *candidates = new list<ExonCandidate*>;
    for(map<int_fast64_t, ExonCandidate*>::iterator ecit=ecs.begin(); ecit!=ecs.end(); ecit++){
	candidates->push_back(ecit->second);
    }
    exoncands[s] = candidates;
    ecs.clear(); // not needed anymore
    cout << "Found " << exoncands[s]->size() << " ECs on species " << rsa->getSname(s) << endl; 
}


/**
 * createOrthoExons
 */
void GeneMSA::createOrthoExons(map<int_fast64_t, list<pair<int,ExonCandidate*> > > &alignedECs, Evo *evo, float consThres, int minAvLen) {

    //cout << "Creating ortho exons for alignment" << endl << *alignment << endl;
    int k = alignment->rows.size();
    int m = alignment->numFilledRows();; // the number of nonempty rows

    // an ortho exon candidate must have an EC in at least this many species (any subset allowed):
    int minEC = (consThres * m > 2.0)? m * consThres + 0.9999 : 2;
    cout << "OEs in this gene range must have at least " << minEC << " ECs" << endl;

    map<int_fast64_t, list<pair<int,ExonCandidate*> > >::iterator aec;
    unordered_map<bit_vector,PhyloTree*>::iterator topit;
    /*
     * Create one ortho exon candidate for each key to which at least minEC exon candidates mapped 
     */
    int numOE = 0;
    for (aec = alignedECs.begin(); aec != alignedECs.end(); ++aec){
	if (aec->second.size() >= minEC){
	    float avLen = 0.0;
	    OrthoExon oe(aec->first, numSpecies());
	    bit_vector bv(k,0);
	    oe.orthoex.resize(k, NULL);
	    for (list<pair<int,ExonCandidate*> >::iterator it = aec->second.begin(); it != aec->second.end(); ++it){
		int s = it->first;
		ExonCandidate *ec = it->second;
		// cout << rsa->getSname(s) << "\t" << ec->getStart() + offsets[s] << ".." << ec->getEnd() + offsets[s] << "\t" << *ec << endl;
		if (oe.orthoex[s])
		    throw ProjectError("createOrthoExons: Have two exon candidates from the same species " 
				       + rsa->getSname(s) + " with the same key " + itoa(aec->first));
		bv[s]=1;
		oe.orthoex[s] = ec;
		avLen += ec->len();
	    }
	    avLen /= aec->second.size(); // compute average length of exon candidates in oe
	    if (avLen >= minAvLen){
		oe.ID = orthoExonID;
		oe.setBV(bv);
		// link OE to tree topology
		topit = topologies.find(bv);
		if(topit == topologies.end()){ // insert new topology
		    PhyloTree *t = new PhyloTree(*tree);
		    t->prune(bv,evo);
		    topologies.insert(pair<bit_vector,PhyloTree*>(bv,t));
		    oe.setTree(t);
		}
		else{
		    oe.setTree(topit->second);
		}
		oe.setContainment(0);
		orthoExonID++;
		numOE++;
		orthoExonsList.push_back(oe);
	    }
	}
    }
    alignedECs.clear(); // not needed anymore
    /*
     * Determine 'containment' for each OE: The average number of extra bases (per species) of the largest OE (y) that includes this OE (x) in frame.
     * Idea: A large containment is a sign that OE is actually not true.
     * species
     *  1       xxxxxxxxxxxxxxxxxxxxxxxxx           containment = 5
     *  1     YYyyyyyyyyyyyyyyyyyyyyyyyyyYY
     *  2       xxxxxxxxxxxxxxxxxxxxxxxxx
     *  2     YYyyyyyyyyyyyyyyyyyyyyyyyyyYYYY
     *  3   yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy     
     *  Containing OE y must have ECs for all species where x has ECs, and may have ECs for additional species.
     */
    list<OrthoExon>::iterator oeYit, oeXit, ovlpBeginIt;
    ovlpBeginIt = orthoExonsList.begin();
    for (oeYit = orthoExonsList.begin(); oeYit != orthoExonsList.end();	++oeYit){
	int YaliEnd = oeYit->getAliEnd();
	int YaliStart = oeYit->getAliStart();
	while (ovlpBeginIt != orthoExonsList.end() && ovlpBeginIt->getAliStart() < YaliStart)
	    ++ovlpBeginIt;
	// X ranges from the first OE with the same start as Y to the last OE with the start before Y's end
	oeXit = ovlpBeginIt;
	while (oeXit != orthoExonsList.end() && oeXit->getAliStart() <= YaliEnd){
	    if (oeXit != oeYit && // same OE would yield a containment of 0 anyways
		oeXit->getAliEnd() <= YaliEnd // x is contained in y in the alignment
		&& isOnFStrand(oeXit->getStateType()) == isOnFStrand(oeYit->getStateType())){ // same strand
		size_t containment = 0;
		bool contained = true;
		for (size_t s=0; s < numSpecies() && contained; s++) {
		    if (oeXit->orthoex[s]) {
			if (oeYit->orthoex[s]){	
			    int leftOverhang = oeXit->orthoex[s]->getStart() - oeYit->orthoex[s]->getStart();
			    int rightOverhang = oeYit->orthoex[s]->getEnd() - oeXit->orthoex[s]->getEnd();
			    bool frameCompatible = ((oeYit->orthoex[s]->getFirstCodingBase() - oeXit->orthoex[s]->getFirstCodingBase()) % 3 == 0);
			    if (leftOverhang >= 0 && rightOverhang >= 0 && frameCompatible)
				containment += leftOverhang + rightOverhang;
			    else 
				contained = false;	
			} else {
			    contained = false;
			}
		    }
		}
		if (contained) {
		    // x contains y in above sense
		    containment /= oeXit->numExons(); // average over species present in x
		    if (oeXit->getContainment() < containment)
			oeXit->setContainment(containment);
		}
	    }
	    oeXit++;
	}
    }
    cout << "Found " << numOE << " ortho exons" << endl;
}


void GeneMSA::openOutputFiles(string outdir){
    
    if (Constant::exoncands) // output of exon candidates into a gff file requested
	exonCands_outfiles.resize(tree->numSpecies());
    orthoExons_outfiles.resize(tree->numSpecies());
    geneRanges_outfiles.resize(tree->numSpecies());
    omega_outfiles.resize(tree->numSpecies());
    vector<string> species;
    tree->getSpeciesNames(species);
    for (int i=0; i<tree->numSpecies(); i++) {
        string file_exoncand = outdir + "exonCands." + species[i] + ".gff3";
        ofstream *os_ec = NULL;
	if (Constant::exoncands){
	    os_ec = new ofstream(file_exoncand.c_str());
	    if (os_ec) {
		exonCands_outfiles[i] = os_ec;
		(*os_ec) << PREAMBLE << endl;
		(*os_ec) << "#\n#-----  exon candidates  -----" << endl << "#" << endl;
	    }
        }
        string file_geneRanges = outdir + "geneRanges." + species[i] + ".gff3";
        ofstream *os_gr = new ofstream(file_geneRanges.c_str());
        if (os_gr!=NULL) {
            geneRanges_outfiles[i]=os_gr;
            (*os_gr) << PREAMBLE << endl;
            (*os_gr) << "#\n#-----  possible gene ranges  -----" << endl << "#" << endl;
        }
        string file_orthoexon = outdir + "orthoExons." + species[i] + ".gff3";
        ofstream *os_oe = new ofstream(file_orthoexon.c_str());
        if (os_oe) {
            orthoExons_outfiles[i]=os_oe;
            (*os_oe) << PREAMBLE << endl;
            (*os_oe) << "#\n#----- ortholog exons  -----" << endl << "#" << endl;
        }
        /*string file_omega = outdir + "omegaExons." + species[i] + ".gff3";
        ofstream *os_omega = new ofstream(file_omega.c_str());
        if (os_omega) {
            omega_outfiles[i]=os_omega;
            (*os_omega) << PREAMBLE << endl;
            (*os_omega) << "#\n#----- exons with a dN/dS ratio smaller than one -----" << endl << "#" << endl;
	}*/
    }
}

void GeneMSA::printStats(){
    for(int s=0; s<numSpecies(); s++){
	cout << rsa->getSname(s) << "\t";
	if (alignment->rows[s])
	    cout << alignment->rows[s]->strand << "\t" << getStart(s) << "\t" << getEnd(s);
	cout << endl;
    }
}

// appends the gene range of each species to the file 'geneRanges.speciesnames.gff3'
void GeneMSA::printGeneRanges() {
    for (size_t s=0; s < numSpecies(); s++) {
	ofstream &fstrm = *geneRanges_outfiles[s]; // write to 'geneRanges.speciesname[s].gff3'
	if (getStart(s) >= 0) {
	    fstrm << getSeqID(s) << "\tGeneRange\t" << "exon\t" << getStart(s) + 1 << "\t" << getEnd(s) + 1 << "\t0\t"
		  << getStrand(s) << "\t" << ".\t" << "Name=" << geneRangeID;
	    fstrm << ";Note=" << alignment->getSignature() << endl;
	}
    }
    geneRangeID++;
}

// writes the exon candidates of all species into the file 'exonCands.species.gff3'
void GeneMSA::printExonCands() {
    exonCandID.resize(numSpecies(), 1);

    if (!exoncands.empty()) {
        for (int s=0; s < numSpecies(); s++) {
	    ofstream &fstrm = *exonCands_outfiles[s]; // write to 'exonCands.speciesname[i].gff3'
	    list<ExonCandidate*>* sec = exoncands[s];
            if (sec) {
                fstrm << "# sequence:\t" << rsa->getSname(s) << "\t" << getStart(s) + 1 << "-" 
		      << getEnd(s) + 1 << "  " << getEnd(s) - getStart(s) << "bp" << endl;
                for (list<ExonCandidate*>::iterator ecit = sec->begin(); ecit != sec->end(); ++ecit) {
                    fstrm << getSeqID(s)<< "\tEC\t" << "exon\t";
                    if (getStrand(s) == plusstrand) {
                        fstrm << (*ecit)->begin + offsets[s] + 1 << "\t" << (*ecit)->end + offsets[s] + 1 
			      << "\t" << (*ecit)->score << "\t";
                    } else {
			int chrLen = rsa->getChrLen(s, getSeqID(s));
                        fstrm << chrLen - ((*ecit)->end + offsets[s]) << "\t"
			      << chrLen - ((*ecit)->begin+ offsets[s]) << "\t"
			      << (*ecit)->score << "\t";
                    }
		    // the gff strand of the exon is the "strand product" of the alignment strand and exon type strand
		    // e.g. "-" x "-" = "+" << '+' << "\t";
		    fstrm << ((isPlusExon((*ecit)->type) == (getStrand(s) == plusstrand))? '+' : '-');
                    fstrm << "\t" << (*ecit)->gff3Frame() << "\t" << "ID=" << exonCandID[s] << ";"
			  << "Name=" <<stateExonTypeIdentifiers[(*ecit)->type] << endl;
		    // TODO: adjust type on reverse alignment setrand (compate comment in printSingleOrthoExon)

                    exonCandID[s]++;
                }
            } else {
                fstrm << "#  no exon candidates found " << endl;
            }
        }
    } else {
        cout << "#  no exon candidates found at all" << endl;
    }
}

// writes all ortholog exons of all species in the files 'orthoExons.species.gff3'
// orthoexons are sorted by alignment start coordinate
void GeneMSA::printOrthoExons() {
    if (orthoExonsList.empty())
	return;
    for (list<OrthoExon>::iterator oeit = orthoExonsList.begin(); oeit != orthoExonsList.end(); ++oeit)
	printSingleOrthoExon(*oeit, true);
}

// writes the ortholog exons on one OrthoExon into the files 'orthoExons.species.gff3'
// files: write each one to a file for its species, if false to stdout
void GeneMSA::printSingleOrthoExon(OrthoExon &oe, bool files) {
    bool GBrowseStyle = false; // for viewing in GBrowse use this style
    streambuf *stdout = cout.rdbuf();
    string stored_pattern = oe.getStoredLabelpattern();
    string current_pattern = oe.getCurrentLabelpattern(); 
    for (int s=0; s < numSpecies(); s++) {
	ExonCandidate *ec = oe.orthoex.at(s);
	if (files)
	    cout.rdbuf(orthoExons_outfiles[s]->rdbuf()); // write to 'orthoExons.speciesname[s].gff3'
        if (ec != NULL) {
	    cout << getSeqID(s) << "\tOE1\t" << "exon" << "\t";
            if (getStrand(s) == plusstrand){ // strand of alignment
		cout << ec->begin + offsets[s]+1 << "\t" << ec->end + offsets[s]+1;
            } else {
		int chrLen = rsa->getChrLen(s, getSeqID(s));
                cout << chrLen - (ec->end + offsets[s]) << "\t" << chrLen - (ec->begin + offsets[s]);
            }
	    cout << "\t" << ec->score << "\t" 
		// the gff strand of the exon is the "strand product" of the alignment strand and exon type strand
		// e.g. "-" x "-" = "+"
		 << ((isPlusExon(ec->type) == (getStrand(s) == plusstrand))? '+' : '-');
            cout << "\t" << ec->gff3Frame() << "\t" << "ID=" << oe.ID << ";Name=" << oe.ID << ";Note=" 
		 << stateExonTypeIdentifiers[ec->type]; // TODO: reverse type if alignment strand is "-"
	    if (GBrowseStyle)
		cout << "|" << oe.numExons();
	    else 
		cout << ";n=" << oe.numExons();
	    if (oe.getOmega() >= 0.0){
		if (GBrowseStyle)
		    cout << "|" << oe.getOmega();
		else 
		    cout << ";omega=" << oe.getOmega();
	    }
	    if (oe.getEomega() >= 0.0){
		if (GBrowseStyle)
		    cout << "|" << oe.getEomega();
		else
		    cout << ";Eomega=" << oe.getEomega();
	    }
	    if (oe.getVarOmega() >= 0.0){
		if (GBrowseStyle)
		    cout << "|" << oe.getVarOmega();
		else
		    cout << ";VarOmega=" << oe.getVarOmega();
	    }
	    if (oe.getSubst() >= 0){ // number of substitutions
		if (GBrowseStyle)
		    cout << "|" << oe.getSubst();
		else
		    cout << ";subst=" << oe.getSubst();
	    }
	    if (oe.getConsScore() >= 0.0){ // conservation score
		if (GBrowseStyle)
		    cout << "|" << oe.getConsScore();
		else
		    cout << ";cons=" << oe.getConsScore();
	    }
	    if (oe.getDiversity() >= 0.0){ // diversity
		if (GBrowseStyle)
		    cout << "|" << oe.getDiversity();
		else
		    cout << ";div=" << oe.getDiversity();
	    }
	    if (GBrowseStyle)
		cout << "|" << oe.getContainment();
	    else
		cout << ";containment=" << oe.getContainment();
	    if (GBrowseStyle)
                cout << "|" << stored_pattern <<":"<<current_pattern;
	    else
                cout << ";labelpattern=" << stored_pattern <<":"<< current_pattern;
	    cout << endl;
        }
    }
   cout.rdbuf(stdout); // reset to standard output again 
}

/** 
 * Two codons are considered aligned, when all 3 of their bases are aligned with each other.
 * Note that not all bases of an ExonCandidate need be aligned.
 * example input (all ECs agree in phase at both boundaries)
 *        
 *                 a|c - - t t|g a t|g t c|g a t|a a 
 *                 a|c - - c t|a a - - - c|a n c|a g
 *                 g|c g - t|t g a|- g t c|g a c|a a
 *                 a|c g t|t t g|a t - t|c g a|c - a
 *                 a|c g - t|t g a|t g t|t g a|- a a
 *                   ^                       ^
 * firstCodonBase    |                       | lastCodonBase (for last species)
 * example output: (stop codons are excluded for singe and terminal exons)
 *                 - - -|c t t|- - -|g t c|- - -|g a t
 *                 - - -|c c t|- - -|- - -|- - -|a n c
 *                 c g t|- - -|t g a|g t c|- - -|g a c
 *                 - - -|- - -|- - -|- - -|c g a|- - -
 *                 c g t|- - -|t g a|- - -|t g a|- - -
 *
 */
vector<string> GeneMSA::getCodonAlignment(OrthoExon const &oe, vector<AnnoSequence*> const &seqRanges,
					  const vector<vector<fragment>::const_iterator > &froms, map<unsigned, vector<int> > *alignedCodons, bool generateString, vector<vector<int> > *posStoredCodons) {
    //printSingleOrthoExon(oe, false);
    //cout<<"OE("<<oe.ID<<") start: "<<oe.getAliStart()<<" end: "<<oe.getAliEnd()<<endl;
    int k = alignment->rows.size();
    vector<string> rowstrings(k, "");
    // consider only codon columns with a number of codons at least this fraction of the nonempty rows 
   
    /*
     * Store for each aligned codon the triplet of alignment columns encoded in a single long integer
     * key = aliPosOf1stBase * 2^8 + gapsTo2ndBase * 2^4 + gapsTo3rdBase
     * This assumes even on a rare 32 bit machine only that the alignment is shorter than 16,777,216,
     * and that gaps within a codon are at most 15bp. Where this is violated, wrong codon alignments
     * may happen.
     * Values of alignedCodons are pairs of 1) species index s and 2) the chromosomal position of the 
     * first codon base.
     */
    map<unsigned, vector<int> > ac;
    if(alignedCodons == NULL)
	alignedCodons = &ac;
    map<unsigned, vector<int> >::iterator acit;
    
    long aliPosOf1stBase, aliPosOf2ndBase, aliPosOf3rdBase;
    int chrCodon1;
    // Map all codons to alignment positions, where possible. This search in LINEAR in the length
    // of all exon candidates and the number of all alignment fragments.
    for (size_t s=0; s<k; s++){
	if (alignment->rows[s] == NULL || oe.orthoex[s] == NULL)
	    continue;
	//cout<<"species "<<s<<"|"<<getSeqID(s);
	AlignmentRow *row = alignment->rows[s];
	ExonCandidate *ec = oe.orthoex[s];
	int firstCodonBase = offsets[s] + ec->getFirstCodingBase();
	int lastCodonBase = offsets[s] + ec->getLastCodingBase();
	//cout<<" firstBase: "<<ec->begin<<" firstCodonBase: "<<firstCodonBase<<" lastCodonBase: "<<lastCodonBase<<" frame: "<<(firstCodonBase % 3)<<endl;
	if ((lastCodonBase - firstCodonBase + 1) % 3 != 0)
	    throw ProjectError("Internal error in getCodonAlignment: frame and length inconsistent.");

	if(posStoredCodons != NULL){
	    if((*posStoredCodons)[s][firstCodonBase % 3] >= lastCodonBase )
		continue;
	    else{
		if(firstCodonBase < (*posStoredCodons)[s][firstCodonBase % 3] + 1)
		    firstCodonBase = (*posStoredCodons)[s][firstCodonBase % 3] + 1;
		(*posStoredCodons)[s][firstCodonBase % 3] = lastCodonBase;
	    }
	}
	vector<fragment>::const_iterator from = froms[s];
	for(chrCodon1 = firstCodonBase; chrCodon1 <= lastCodonBase - 2; chrCodon1 += 3){
	    // chrCodon1 is the chromosomal position of the first base in the codon
	   
	    // go the the first fragment that may contain chrCodon1
	    while (from != row->frags.end() && from->chrPos + from->len - 1 < chrCodon1)
		++from;
	    if (from == row->frags.end())
		break; // have searched beyond the codon => finished
	    aliPosOf1stBase = row->getAliPos(chrCodon1, from);
	    if (aliPosOf1stBase >= 0){ // first codon base mappable
		aliPosOf2ndBase = row->getAliPos(chrCodon1 + 1, from);
		if (aliPosOf2ndBase >= 0){
		    aliPosOf3rdBase = row->getAliPos(chrCodon1 + 2, from);
		    if (aliPosOf3rdBase >= 0){
			// all the codon bases were mappable, store codon in the map
			long key = (aliPosOf1stBase << 8) + ((aliPosOf2ndBase - aliPosOf1stBase - 1) << 4)
			    + (aliPosOf3rdBase - aliPosOf2ndBase - 1);
			//cout << chrCodon1 << " " << aliPosOf1stBase << " " << (aliPosOf2ndBase - aliPosOf1stBase - 1) << " " 
			//     << (aliPosOf3rdBase - aliPosOf2ndBase - 1) << " " << key << endl;
			acit = alignedCodons->find(key);
			if (acit == alignedCodons->end()){ // insert new vector
			    vector<int> cod(k, -1); // -1 missing codon
			    cod[s] = chrCodon1;
			    alignedCodons->insert(pair<int,vector<int> >(key, cod));
			    //cout<<key<<"\t";;
			} else {// append new entry to existing list
			    if (acit->second[s] >= 0) // remove this later
				throw ProjectError("Wow! Another codon has mapped to the same key!");
			    acit->second[s] = chrCodon1;
			}
		    }
		}
	    }
	}
	//cout<<endl;
    }
    
    if(generateString){
	float minAlignedCodonFrac = 0.3;
	int m = alignment->numFilledRows();
	int minAlignedCodons = (m * minAlignedCodonFrac > 2)? m * minAlignedCodonFrac + 0.9999 : 2;
	// Must have at least 'minAlignedCodons' codons in any codon column

	/*
	 * Create one codon alignment column for each key to which at least minAlignedCodons mapped
	 */ 
	for (acit = alignedCodons->begin(); acit != alignedCodons->end(); ++acit){
	    int numCodons = 0;
	    for(size_t s=0; s<k; s++)
		if (acit->second[s] >=0)
		    numCodons++;
	    if (numCodons >= minAlignedCodons){
		for (size_t s=0; s<k; s++){
		    chrCodon1 = acit->second[s]; // sequence position
		    if (chrCodon1 >= 0)
			rowstrings[s] += string(seqRanges[s]->sequence + chrCodon1 - offsets[s], 3);
		    else 
			rowstrings[s] += "---";
		}
	    }
	}

	if (!isOnFStrand(oe.getStateType())){ // reverse complement alignment
	    for (size_t s=0; s<k; s++)
		reverseComplementString(rowstrings[s]); 
	}

	/*cout << "codon alignment:" << endl;
	  int maxSnameLen = rsa->getMaxSnameLen();
	  int maxSeqIDLen = alignment->getMaxSeqIdLen();
	  for (size_t s=0; s<k; s++)
	  cout << setw(maxSnameLen) << rsa->getSname(s) << "\t" << setw(maxSeqIDLen) << getSeqID(s) << "\t" << rowstrings[s] << endl;
	*/
    }
    return rowstrings;
}


// computes and sets the Omega = dN/dS attribute to all OrthoExons
void GeneMSA::computeOmegas(vector<AnnoSequence*> const &seqRanges) {
    // int subst = 0;
    // Initialize for each species the first fragment froms[s] that 
    // is not completely left of the current OrthoExon.
    // This exploits the fact that both fragments and OrthoExons are sorted
    // left-to-right in the alignment.
  

    vector<vector<fragment>::const_iterator > froms(numSpecies());
    for (size_t s=0; s < numSpecies(); s++)
	if (alignment->rows[s])
	    froms[s] = alignment->rows[s]->frags.begin();
    for (list<OrthoExon>::iterator oe = orthoExonsList.begin(); oe != orthoExonsList.end(); ++oe){
	// move fragment iterators to start of exon candidates
	for (size_t s=0; s < numSpecies(); s++)
	    if (alignment->rows[s] && oe->orthoex[s])
		while(froms[s] != alignment->rows[s]->frags.end() 
		      && froms[s]->chrPos + froms[s]->len - 1 < offsets[s] + oe->orthoex[s]->getStart())
		    ++froms[s];
		
	vector<string> rowstrings = getCodonAlignment(*oe, seqRanges, froms);
	double omega;
	// TODO: scale branch lengths to one substitution per codon per time unit
	// cout << "OE" << endl;
	//	printSingleOrthoExon(*oe, false);
	// int subst = 0;
	// omega = codonevo->estOmegaOnSeqTuple(rowstrings, tree, subst);
	//	cout << "omega=" << omega << endl;
	// oe->setSubst(subst);
	omega = codonevo->graphOmegaOnCodonAli(rowstrings, tree);
	oe->setOmega(omega);
    }
}


// data to store in the map aliPos. for positions in the alignment store start and end of orthoExons and bitvectors
struct posElements{
    vector<OrthoExon*> oeStart;
    vector<OrthoExon*> oeEnd; 
};

string printBV(bit_vector bv){
    string bvs="";
    for(bit_vector::iterator b = bv.begin(); b != bv.end(); b++){
	if(*b)
	    bvs+= "1|";
	else
	    bvs+= "0|";
    }
    return bvs;
}

string printRFC(vector<int> rfc){
    string rfcs="";
    for(int i=0; i<rfc.size(); i++){
	rfcs+=itoa(rfc[i])+"|";
    }
    return rfcs;
}


void printTest(map<unsigned, vector<int> > alignedCodons, map<int, posElements> aliPos){

    cout<<"--- list of keys for codon algnment ---"<<endl;
    unordered_map<bit_vector, int> bvCount;
    map<int, posElements>::iterator aliPosIt = aliPos.begin();
    for(map<unsigned, vector<int> >::iterator codonIt = alignedCodons.begin(); codonIt != alignedCodons.end(); codonIt++){
	cout<<"codon position in alignment(1): "<<(codonIt->first >> 8)<<" oe start or end in alignment(2): "<<aliPosIt->first<<endl;
	map<unsigned, vector<int> >::iterator nextCodonIt = codonIt;
	nextCodonIt++;
	while((unsigned)aliPosIt->first >= (codonIt->first >> 8) && (unsigned)aliPosIt->first <= (nextCodonIt->first >> 8)){
	    for(int i=0; i<aliPosIt->second.oeStart.size(); i++)
		cout<<"oeStart: "<<aliPosIt->second.oeStart[i]->getAliStart()<<":"<<aliPosIt->second.oeStart[i]->getAliEnd()<<endl;
	    for(int i=0; i<aliPosIt->second.oeEnd.size(); i++)
		cout<<"oeEnd: "<<aliPosIt->second.oeEnd[i]->getAliStart()<<":"<<aliPosIt->second.oeEnd[i]->getAliEnd()<<endl;
	    aliPosIt++;
	}
    }
}

cumValues* GeneMSA::findCumValues(bit_vector bv, vector<int> rfc){
    cout<<"in findCumValues, Parameters are "<<printBV(bv)<<" : "<<printRFC(rfc)<<endl;
    for(int b = 0; b < bv.size(); b++)
	if(! bv[b])
	    rfc[b] = -1;
    //cout<<"in findCumValues, Parameters after reduction "<<printBV(bv)<<" : "<<printRFC(rfc)<<endl;
    //  vector<cumValues*> allMatchingRFC;
    unordered_map<bit_vector, vector<pair<vector<int>, cumValues> > >::iterator it = cumOmega.find(bv);
    if(it == cumOmega.end())
	throw ProjectError("Internal error in findCumValues(): requested bit vector does not exist!");
    cout<<"bitvector in cumOmega: "<<printBV(it->first)<<endl;
    //cout<<"bitvector has "<<it->second.size()<<" rfcs"<<endl;
    for(int i = 0; i < it->second.size(); i++){
	// cout<<"rfc number "<<i<<endl;
	bool isRFC = true;
	cout<<"rfc in cumOmega: "<<printRFC(it->second[i].first)<<endl;
	for(int j = 0; j < it->second[i].first.size(); j++){
	    if(it->second[i].first[j] != rfc[j] && rfc[j] != -1){
	      //cout<<"this rfc is not the one we're looking for"<<endl;
		isRFC = false;
		break;
	    }	
	}
	if(isRFC){
	    //cout<<"return cumValues: "<<it->second[i].second.omega<<endl;
	    return &it->second[i].second;
	}
    }
    return NULL;
}


void GeneMSA::printCumOmega(){
    cout<<"-------------- cumOmega -------------------"<<endl;
    for(unordered_map<bit_vector, vector<pair<vector<int>, cumValues> > >::iterator coit = cumOmega.begin(); coit != cumOmega.end(); coit++){
	cout<<printBV(coit->first)<<endl;
	for(int i = 0; i < coit->second.size(); i++){
	     cout<<"\t"<<printRFC(coit->second[i].first)<<endl;
	}
    }
    cout<<"--------------------------------------------"<<endl;
}


void GeneMSA::computeOmegasEff(vector<AnnoSequence*> const &seqRanges) {
    cout<<"computing omega for each ortho exon."<<endl;

    // treat forward and reverse strand seperately (might be done more efficiently)
    for( bool plusStrand : {true, false}){
	if(plusStrand){
	    cout<<"--- processing ortho exons on forward strand ---"<<endl;
	}else{
	    cout<<"--- processing ortho exons on reverse strand ---"<<endl;
	    }
	vector<vector<fragment>::const_iterator > froms(numSpecies());
	for (size_t s=0; s < numSpecies(); s++)
	    if (alignment->rows[s])
		froms[s] = alignment->rows[s]->frags.begin();
        
	cumOmega.clear();
	map<int, posElements> aliPos;  // contains all positions where either an orthoExon or a bitvector starts or ends
	map<unsigned, vector<int> > alignedCodons;
	alignedCodons.insert(pair<unsigned, vector<int> >(0,vector<int>(numSpecies(),-1))); // guaratee that codon alignment starts before first OrthoExon
	vector<vector<int> > posStoredCodons(numSpecies(),vector<int>(3,0)); // stores the position of the last codon aligned in getCodonAlignment() for each species and reading frame
    
	for (list<OrthoExon>::iterator oe = orthoExonsList.begin(); oe != orthoExonsList.end(); ++oe){
	  if(isOnFStrand(oe->getStateType()) != plusStrand)
		continue;
	    //printSingleOrthoExon(*oe, false);
	    // store start and end information
	    bool aliStart = true;
	    for (map<int, posElements>::iterator aliPosIt : { aliPos.find(oe->getAliStart()), aliPos.find(oe->getAliEnd()) }) { 
      
		if(aliPosIt == aliPos.end()){
		    posElements pe;
		    pair<map<int, posElements>::iterator, bool> insertResult;
		    if(aliStart)
			insertResult = aliPos.insert(pair<int, posElements>(oe->getAliStart(), pe));
		    else
			insertResult = aliPos.insert(pair<int, posElements>(oe->getAliEnd(), pe));
		    aliPosIt = insertResult.first;
		}
		if(aliStart){
		    aliPosIt->second.oeStart.push_back(&(*oe));
		}else{
		    aliPosIt->second.oeEnd.push_back(&(*oe));
		}
		aliStart = false;
	    }
    
	    // generate codon alignments
	    // move fragment iterators to start of exon candidates                                                                        
	    for (size_t s=0; s < numSpecies(); s++)
		if (alignment->rows[s] && oe->orthoex[s])
		    while(froms[s] != alignment->rows[s]->frags.end()
			  && froms[s]->chrPos + froms[s]->len - 1 < offsets[s] + oe->orthoex[s]->getStart())
			++froms[s];
    
	    getCodonAlignment(*oe, seqRanges, froms, &alignedCodons, false, &posStoredCodons);
	}
  
  
	// Merge processing: Traverse alignment left to right 

	unordered_map<bit_vector, int> bvCount;

	map<int, posElements>::iterator aliPosIt = aliPos.begin();
	if(aliPosIt == aliPos.end()){
	  cout<<"No orthoExons in current gene range!"<<endl;
	  continue;
	}
	  
	map<vector<string>,vector<double> > computedOmegas;

	for(map<unsigned, vector<int> >::iterator codonIt = alignedCodons.begin(); codonIt != alignedCodons.end(); codonIt++){
	    cout<<"codon: "<<(codonIt->first >> 8)<<endl;
	    cout<<"active bit vectors: ";
	    for(unordered_map<bit_vector, int>::iterator bit=bvCount.begin(); bit!=bvCount.end(); bit++){
	      if(bit->second > 0)
		cout<<printBV(bit->first)<<":"<<bit->second<<"\t";
	    }
	    cout<<endl;

	    // update bit_vector constellation
	    while((unsigned)aliPosIt->first <= (codonIt->first >> 8) ){
	      cout<<"next position of aliPos "<<aliPosIt->first<<endl;
		unordered_map<bit_vector, int>::iterator bvit;
		unordered_map<bit_vector, vector<pair<vector<int>, cumValues> > >::iterator coit;
		for(int i=0; i<aliPosIt->second.oeStart.size(); i++){
		      cout<<"ortho exon starts: "<<aliPosIt->second.oeStart[i]->getAliStart()<<":"<<aliPosIt->second.oeStart[i]->getAliEnd()<<endl;
		     cout<<"---bv of ortho exon:  "<<printBV(aliPosIt->second.oeStart[i]->getBV())<<endl<<"---rfc of ortho exon: "<<printRFC(aliPosIt->second.oeStart[i]->getRFC(offsets))<<endl;
		     if(aliPosIt->second.oeStart[i] == NULL)
		       cout<<"Error in posElement.oestart: Pointer to orthoExon is NULL"<<endl;
		    bvit = bvCount.find(aliPosIt->second.oeStart[i]->getBV());
		    coit = cumOmega.find(aliPosIt->second.oeStart[i]->getBV());
	
		    if(bvit == bvCount.end()){
			pair<unordered_map<bit_vector, int>::iterator,bool> result = bvCount.insert(pair<bit_vector, int>(aliPosIt->second.oeStart[i]->getBV(),0));
			bvit = result.first;
			//if(! result.second)
			    // cout<<"inserting bit vector "<<printBV(bvit->first)<<" into bvcount failed"<<endl;
			//else
			    // cout<<"inserting bit vector "<<printBV(bvit->first)<<" into bvcount done"<<endl;
		    }else{
			//cout<<"bit vector "<<printBV(bvit->first)<<" already inserted in bvcount"<<endl;
			if(coit == cumOmega.end())
			    throw ProjectError("Internal Error in computeOmegaEff(): bit_vector in bvcount but not yet in cumOmega!");
		    }
		    bvit->second++;

		    // add reading frame combination if new one occurs
		    int currRFnum; //position in cumOmega vector
	
		    if(coit == cumOmega.end()){
			vector<pair<vector<int>, cumValues> > vecPair;
			pair<unordered_map<bit_vector, vector<pair<vector<int>, cumValues> > >::iterator, bool> result = cumOmega.insert(pair<bit_vector, vector<pair<vector<int>, cumValues> > >(aliPosIt->second.oeStart[i]->getBV(), vecPair));
			coit = result.first;
			if(! result.second)
			    cout<<"inserting bit vector "<<printBV(coit->first)<<" into cumOmega failed"<<endl;
			/*else
			    cout<<"inserting bit vector "<<printBV(coit->first)<<" into rfCombi done"<<endl;
			*/
			//}else{
			//cout<<"bit vector "<<printBV(coit->first)<<"already exists"<<endl;
		    }
		    bool rfcIncluded = false;
		    cumValues cum;
		    pair<vector<int>, cumValues> oeRFC = make_pair(aliPosIt->second.oeStart[i]->getRFC(offsets),cum);
		    for(int rf = 0; rf < coit->second.size(); rf++){
			if(oeRFC.first == coit->second[rf].first){
			    currRFnum = rf;
			    rfcIncluded = true;
			    cout<<printRFC(oeRFC.first)<<" already in cumOmega ("<<printRFC(coit->second[rf].first)<<")"<<endl;
			    break;
			}
		    }
		    if(! rfcIncluded){
			coit->second.push_back(oeRFC);
			cout<<"inserting RFC: "<<printRFC(oeRFC.first)<<" to bit_vector "<<printBV(coit->first)<<endl;
			currRFnum = coit->second.size() - 1;
		    }

		    // store cumulative values at the begining of an OrthoExon
		    cumValues cv = coit->second[currRFnum].second;
		    aliPosIt->second.oeStart[i]->setOmega(&cv.logliks, codonevo, true);
		}

		for(int i=0; i<aliPosIt->second.oeEnd.size(); i++){
		    cout<<"ortho exon ends: "<<aliPosIt->second.oeEnd[i]->getAliStart()<<":"<<aliPosIt->second.oeEnd[i]->getAliEnd()<<endl;
		    bvit = bvCount.find(aliPosIt->second.oeEnd[i]->getBV());
		    if(bvit == bvCount.end())
			throw ProjectError("Error in computeOmegaEff(): bit_vector ends that has never started!");
		    // calculate omega of ortho exon from cumulative sum
		    cumValues *cv = findCumValues(aliPosIt->second.oeEnd[i]->getBV(), aliPosIt->second.oeEnd[i]->getRFC(offsets));
		    //cout<<"pointer to cumValues: "<<cv->omega<<endl;
		    aliPosIt->second.oeEnd[i]->setOmega(&cv->logliks, codonevo, false);
		    bvit->second--;
		}
		aliPosIt++;
	    }

	    // compute omega for current codon alignment
    
	    // generate array of strings representing one codon alignment
	    vector<string> codonStrings(numSpecies(),"");
	    float minAlignedCodonFrac = 0.3;
	    int m = alignment->numFilledRows();
	    int minAlignedCodons = (m * minAlignedCodonFrac > 2)? m * minAlignedCodonFrac + 0.9999 : 2;
	    // Must have at least 'minAlignedCodons' codons in any codon column
	    int numCodons = 0;
	    for(size_t s = 0; s < numSpecies(); s++)
		if (codonIt->second[s] >=0)
		    numCodons++;
	    if(numCodons >= minAlignedCodons){
		vector<int> rfc(numSpecies(),-1); // reading frame combination of current codon alignment
		for (size_t s = 0; s < numSpecies(); s++){
		    int chrCodon1 = codonIt->second[s]; // sequence position
		    if (chrCodon1 >= 0){
			codonStrings[s] = string(seqRanges[s]->sequence + chrCodon1 - offsets[s], 3);
			rfc[s] = chrCodon1 % 3;
		    }
		    else 
			codonStrings[s] = "---";
		}
      
		if (! plusStrand){ // reverse complement alignment
		    for (size_t s = 0; s < numSpecies(); s++)
			reverseComplementString(codonStrings[s]); 
		}
      
		// for one alignment position compute omega for all aktive bit_vectors in the correct reading frame combination
		cout<<"compute omega for RFC "<<printRFC(rfc)<<endl;
		bool foundBV = false;
		for(unordered_map<bit_vector, int>::iterator bvit = bvCount.begin(); bvit != bvCount.end(); bvit++){
		    if(bvit->second == 0)
			continue;
		    //  cout<<"next Bitvector in bvcount "<<printBV(bvit->first)<<":"<<bvit->second<<endl;
		    cumValues *cv = findCumValues(bvit->first, rfc);    
		    //cout<<"after findCumValues"<<endl;
		    if(cv != NULL){
			// call pruning algo only once for every codonStrings and store omega in map                                                    
			vector<double> loglik;
			map<vector<string>,vector<double> >::iterator oit = computedOmegas.find(codonStrings);
			if(oit==computedOmegas.end()){
			    loglik = codonevo->loglikForCodonTuple(codonStrings, tree);
			    computedOmegas.insert(pair<vector<string>,vector<double> >(codonStrings,loglik));
			}else{
			    loglik = oit->second;
			}
			//cout<<"omega: "<<omega<<endl;
			// store cumulative sum of omega, omega squared and one
			cv->addLogliks(&loglik);
			foundBV=true;
			//printCumOmega();
		    }
		}
		if(! foundBV)
		    cout<<"no Bitvector with given RFC found!"<<endl;
	    }
	}
	printCumOmega();
    }
}


// calculate a columnwise conservation score and output it (for each species) in wiggle format
void GeneMSA::printConsScore(vector<AnnoSequence*> const &seqRanges, string outdir){

    vector<vector<fragment>::const_iterator > fragsit(numSpecies());
    vector<size_t> seqPos(numSpecies(),0); 
    for (size_t s=0; s < numSpecies(); s++){
	if (alignment->rows[s])
	    fragsit[s] = alignment->rows[s]->frags.begin();
    }
    vector<double> consScore; // vector of conservation scores for each position in the alignment
    for(size_t i = 0; i < alignment->aliLen; i++){
	int a=0,c=0,t=0,g=0; // number of bases of each type in one alignment column
	for(size_t j = 0; j < alignment->rows.size(); j++){
	    AlignmentRow *row = alignment->rows[j];
	    if(row && fragsit[j] != row->frags.end()){
		vector<fragment>::const_iterator it = fragsit[j];
		if( i >= it->aliPos && i <= it->aliPos + it->len - 1){ // character in i-th column, j-th row is not a gap
		    int pos = it->chrPos - offsets[j] + seqPos[j];
                    if(pos < 0 || pos >= seqRanges[j]->length)
                        throw ProjectError("Internal error in GeneMSA::printConsScore: trying to read position" + itoa(pos+1) + "in sequence " + seqRanges[j]->seqname + ".");
		    const char* base = seqRanges[j]->sequence + pos;
		    switch(*base){
		    case 'a': a++; break;
		    case 'c': c++; break;
		    case 't': t++; break;
		    case 'g': g++; break;
		    }
		    seqPos[j]++;
		}
		if(i == it->aliPos + it->len - 1){ // reached the end of the current fragment, move iterator to next fragment
		    ++fragsit[j];
		    seqPos[j]=0;
		}
	    }
	}
	consScore.push_back(calcColumnScore(a,c,t,g));
    }
    // calcluate conservation score for each HECT
    for (list<OrthoExon>::iterator oe = orthoExonsList.begin(); oe != orthoExonsList.end(); ++oe){
	double oeConsScore=0.0;
	int oeAliStart = oe->getAliStart();
	int oeAliEnd = oeAliStart + oe->getAliLen();
	for(int pos = oeAliStart; pos <= oeAliEnd; pos++){
	    if (pos > alignment->aliLen || pos < 0)
		throw ProjectError("Internal error in printConsScore: alignment positions of HECTs and geneRanges are inconsistent.");
	    oeConsScore+=consScore[pos];
	}
	oeConsScore/=(oeAliEnd-oeAliStart+1); // average over all alignment columns within a HECT
	oe->setConsScore(oeConsScore);
    }
    // output for each geneRange and each species a conservation track in wiggle format
    consToWig(consScore, outdir);
}

// calculates a conservation score for a single alignment column
double GeneMSA::calcColumnScore(int a, int c, int t, int g){ // input: number of a,c,t and g's in one alignment column
    int N = numSpecies();
    /* frequency of the most frequent nucleotide type divided by the total number of species
     *double max = 0.0;
     *max = a > c ? a : c;
     *max = max > t ? max : t;
     *max = max > g ? max : g;
     *return max/N;
     */
    // entropy (base 4) weighted by the percentage of rows present, scores range from 0 (non-conserved) to 1 (conserved) 
    double sum=a+c+g+t;
    if(sum == 0)
	return 0.0;
    double probA=a/sum;
    double probC=c/sum;
    double probG=g/sum;
    double probT=t/sum;
    double entropy=0.0;
    if(probA > 0.0 && probA < 1.0)
	entropy-=(probA*log2(probA));
    if(probC > 0.0 && probC < 1.0)
	entropy-=(probC*log2(probC));
    if(probT > 0.0 && probT < 1.0)
	entropy-=(probT*log2(probT));
    if(probG > 0.0 && probG < 1.0)
	entropy-=(probG*log2(probG));
    return (1-(0.5*entropy))*(sum/N);

}

// print conservation score to wiggle file
void GeneMSA::consToWig(vector<double> &consScore, string outdir){

    for (size_t i = 0; i < alignment->rows.size(); i++){
	AlignmentRow *row = alignment->rows[i];
	if(row){
	    string speciesname=rsa->getSname(i);
	    ofstream outfile(outdir + speciesname + ".wig",fstream::app);
	    if (outfile.is_open()){
		outfile<<"track type=wiggle_0 name=\""<<geneRangeID-1<<"\" description=\""<<alignment->getSignature()<<"\""<<endl;
		outfile<<"variableStep chrom="<<row->seqID<<endl;
		for (vector<fragment>::const_iterator it = row->frags.begin(); it != row->frags.end(); ++it){
		    int aliPos=it->aliPos;
		    int chrPos=it->chrPos;
		    while(chrPos < it->chrPos + it->len){
			outfile<<chrPos+1<<" "<<consScore[aliPos]<<endl;
			aliPos++;
			chrPos++;
		    }
		}
	    }
	    outfile.close();
	}
    }
}

void GeneMSA::closeOutputFiles(){
    for (int i=0; i<tree->numSpecies(); i++) {
        if (i < exonCands_outfiles.size() && exonCands_outfiles[i] && exonCands_outfiles[i]->is_open()) {
	    exonCands_outfiles[i]->close();
	    delete exonCands_outfiles[i];
	}
        if (geneRanges_outfiles[i] && geneRanges_outfiles[i]->is_open()) {
	    geneRanges_outfiles[i]->close();
	    delete geneRanges_outfiles[i];
	}
        if (orthoExons_outfiles[i] && orthoExons_outfiles[i]->is_open()) {
	    orthoExons_outfiles[i]->close();
	    delete orthoExons_outfiles[i];
	}
        if (omega_outfiles[i] && omega_outfiles[i]->is_open()) {
	    omega_outfiles[i]->close();
	    delete omega_outfiles[i];
	}
    }
}

void GeneMSA::comparativeSignalScoring(){
    cout << "entering comparativeSignalScoring" << endl;
    ExonCandidate *ec;
    for (list<OrthoExon>::iterator oeit = orthoExonsList.begin(); oeit != orthoExonsList.end(); ++oeit){
	cout << "next OrthoExon:" << endl;
	// loop over all exon candidates belonging to this OrthoExon
	for (int s=0; s < numSpecies(); s++) {
	    ec = oeit->orthoex.at(s); 
	    if (ec != NULL) { // otherwise exon candidate is missing in  species s 
		cout << "next ExonCandidate on sequence " << getSeqID(s) << " from species number "  << s
		     <<  " (= " << rsa->getSname(s) << ")\t";
		cout << *ec; // prints begin, end, type, score, assScore, dssScore
		cout << " type as name = " << stateExonTypeIdentifiers[ec->type];
		cout << endl;
		// assScore is only relevant for internal and terminal exons on both strands
		// dssScore is only relevant for internal and initial exons on both strands
		// ec->getAssScore();
		// ec->getDssScore();
		// for now you will not need these methods, but later we may change the scores
		// according to what you find
		// ec->setAssScore(42);
		// ec->setDssScore(42);		
	    }
	}
    }
    cout << "exiting comparativeSignalScoring" << endl;
}

/* constructTree
 * creates, stores are returns the locus tree for the sequence tuple
 * The locus tree is specific to for the alignment and the aligned sequences.
 * It may be different (also in topology) from the species tree of which we
 * consider only one, because of duplications and paralogy.
 */
LocusTree *GeneMSA::constructTree(){
    // Charlotte Janas playground
    ltree = new LocusTree();
    // construct a tree from the alignment
    return ltree;
} 




