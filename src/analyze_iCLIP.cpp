#include<cstdio>
#include<cstring>
#include<cstdlib>
#include<cassert>
#include<string>
#include<map>
#include<vector>
#include<fstream>
#include<iostream>
#include<sstream>
#include<algorithm>
#include<pthread.h>
#include<unordered_map>

#include "htslib/sam.h"

#include "utils.h"
#include "my_assert.h"

#include "BamAlignment.hpp"
#include "AlignmentGroup.hpp"
#include "SamParser.hpp"
#include "BamWriter.hpp"

#include "PROBerReadModel_iCLIP.hpp"

using namespace std;

bool verbose = true; // define verbose

// position, key for map
struct KeyType {
  char dir;
  int cid, pos;

  KeyType() : dir(0), cid(0), pos(0) {}
  
  KeyType(int cid, char dir, int pos) : dir(dir), cid(cid), pos(pos) {}
  
  bool operator< (const KeyType& o) const {
    if (cid != o.cid) return cid < o.cid;
    if (dir != o.dir) return dir < o.dir; // '+' < '-'
    return pos < o.pos;
  }

  // this function is used to find appropriate window for each site
  int cmp(const KeyType& o, int w) const {
    if (cid != o.cid) return cid < o.cid ? -w - 1 : w + 1;
    if (dir != o.dir) return dir < o.dir ? -w - 1 : w + 1;
    return pos - o.pos;
  }
};

// read count weight, value for map
struct ValueType {
  int c; // number of unique reads
  double weight; // weight, expected read counts at this position from multi-mapping reads
  vector<double*> aligns; // point to expected weight from each alignment

  ValueType() : c(0), weight(0.0) { aligns.clear(); }

  void collect() {
    int s = aligns.size();
    weight = 0.0;
    for (int i = 0; i < s; ++i) weight += *aligns[i];
  }

  void push(double value) {
    int s = aligns.size();
    for (int i = 0; i < s; ++i) *aligns[i] = value;
  }
};

typedef map<KeyType, ValueType> MapType;
typedef map<KeyType, ValueType>::iterator IterType;

MapType posMap; // position map

int n_mhits; // number of multi-mapping reads' hits
double *fracs, *conprbs; // arrays storing multi-mapping reads expected weights and normalized sequencing error probabilities

// multi-mapping reads
struct MultiType {
  int offset, s, c; // offset, the offset at fracs and conprbs array; s, number of alignments; c, number of identical multi reads

  MultiType() : offset(0), s(0), c(0) {}

  MultiType(int offset, int s, int c = 1) : offset(offset), s(s), c(c) {}
};

int n_multi; // number of multi-mapping reads
vector<MultiType> multis; // multi-mapping reads

typedef unordered_map<string, int> HashType;
typedef unordered_map<string, int>::iterator HashIterType;

HashType my_hash;

// for M step use 
struct SiteType {
  ValueType *v; // pointer to ValueType in posMap
  int uc; // unique reads within window size for this site
  int left, right; // window is [left, right]

  SiteType() : v(NULL), uc(0), left(0), right(0) {}
};

int n_msites; // number of multi-read sites
vector<SiteType> sites;

// parameter type, used for multi-threading
struct ParamType {
  int no; // thread number
  int sp, ep; // the start and end position in multis for this thread, [sp, ep)
  int ss, es; // the start and end site in sites [ss, es)  
};

ParamType* params;
pthread_t* threads;
pthread_attr_t attr;
int rc;

// important variables

int model_type;
int w; // half window size
int num_threads; // number of threads we can use
int rounds; // number of iterations

BamAlignment* ba;
AlignmentGroup ag;

PROBerReadModel_iCLIP* model;

/***  string variables  ***/

char imdName[STRLEN], statName[STRLEN];
char multiF[STRLEN], allF[STRLEN]; // multi-mapping reads, all reads
char outF[STRLEN], modelF[STRLEN];

/***  optional arguments and auxiliary variables ***/  

bool bowtie_filter;
int max_hit_allowed; // maximum number of alignments allowed
int min_len; // minimum read length required
int max_len; // maximum read length
bool keep_alignments; // if keep the BAM file
bool last_round;


/****************************************************************************************************/
// Parse alignments


// Filtering from bowtie
inline bool is_filtered_bowtie(AlignmentGroup &ag) {
  ba = ag.getAlignment(0);

  uint8_t *p = NULL;
  char type = 0;

  if (ba->findTag("XM", p, type) && (type == 'i') && (ba->tag2i(p) > 0)) return true;
  if (ba->isPaired() && ba->findTag("XM", p, type, 2) && (type == 'i') && (ba->tag2i(p) > 0)) return true;
  return false;
}

// Categorize reads and learn sequencing error model from uniquely-mapping reads
void parseAlignments(const char* alignF) {
  SamParser* parser;
  BamWriter *writer, *writerBam;

  READ_INT_TYPE N0, N11, N12, N2;

  pair<KeyType, ValueType> my_pair;
  
  parser = new SamParser(alignF);
  
  const char* program_id = parser->getProgramID();
  if (!strcmp(program_id, "Bowtie") || !strcmp(program_id, "bowtie")) bowtie_filter = true;
  
  const bam_hdr_t* header = parser->getHeader();

  sprintf(multiF, "%s_multi.bam", imdName);
  writer = new BamWriter(multiF, header, "PROBer iCLIP intermediate");
  if (keep_alignments) {
    sprintf(allF, "%s_alignments.bam", imdName);
    writerBam = new BamWriter(allF, header);
  }
    
  N0 = N11 = N12 = N2 = 0;
  n_mhits = 0;

  READ_INT_TYPE cnt = 0;
  
  while (parser->next(ag)) {
    if (keep_alignments) writerBam->write(ag);
    
    bool isAligned = ag.isAligned();

    if (ag.isFiltered() || ag.getSeqLength() < min_len || (ag.isPaired() && ag.getSeqLength(2) < min_len) || \
	(isAligned && ag.size() > max_hit_allowed) || (!isAligned && bowtie_filter && is_filtered_bowtie(ag))) {
      ++N2;
    }
    else if (isAligned) {
      // Read is alignable
      if (ag.size() == 1) {
	++N11;
	model->update(ag);

	ba = ag.getAlignment(0);
	my_pair.first.cid = ba->getTid();
	my_pair.first.dir = ba->getDir();
	my_pair.first.pos = ba->getPos();
	posMap.insert(my_pair).first->second.c += 1;
      }
      else {
	++N12;
	n_mhits += ag.size();
	ag.sort_alignments();
	writer->write(ag);
      }
    }
    else {
      // Read is unalignable
      ++N0;
    }

    ++cnt;
    if (verbose && (cnt % 1000000 == 0)) cout<< cnt<< " reads are processed!"<< endl;
  }
  
  if (verbose) cout<< "N0 = "<< N0<< ", N11 = "<< N11<< ", N12 = "<< N12<< ", N2 = "<< N2<< ", n_mhits = "<< n_mhits<< endl<< "parseAlignments is finished."<< endl;

  delete parser;
  delete writer;
  if (keep_alignments) delete writerBam;
}


/****************************************************************************************************/
// Estimate multi-mapping reads sequencing error probabilities


void processMultiReads() {
  SamParser *parser = new SamParser(multiF);

  int offset, size;
  double *p;
  
  pair<KeyType, ValueType> my_pair;
  MultiType multi(0, 0, 1);

  vector<IterType> iters;

  ostringstream key;

  pair<HashIterType, bool> ret;
  
  ag.clear();

  fracs = new double[n_mhits];
  conprbs = new double[n_mhits];
  
  my_hash.clear();

  n_multi = 0; multis.clear();
  
  offset = 0; iters.clear();

  READ_INT_TYPE cnt = 0;

  while (parser->next(ag)) {
    size = ag.size();
    p = conprbs + offset;
    
    model->calcProbs(ag, p);

    key.str("");
    iters.resize(size);
    for (int i = 0; i < size; ++i) {
      	ba = ag.getAlignment(i);
	my_pair.first.cid = ba->getTid();
	my_pair.first.dir = ba->getDir();
	my_pair.first.pos = ba->getPos();
	
	iters[i] = posMap.insert(my_pair).first;

	// generate hash key
	key<< my_pair.first.cid<< my_pair.first.dir<< my_pair.first.pos<< char(int(p[i] * 10.0 + 0.5) + 'A');
    }

    ret = my_hash.insert(make_pair<string, int>(key.str(), n_multi));
    if (ret.second) {
      p = fracs + offset;
      for (int i = 0; i < size; ++i, ++p) {
	*p = 1.0;
	iters[i]->second.aligns.push_back(p);
      }
      
      multi.offset = offset;
      multi.s = size;
      multis.push_back(multi);

      ++n_multi;
      offset += size;
      
    }
    else ++multis[ret.first->second].c;

    ++cnt;
    if (verbose && cnt % 1000000 == 0) cout<< cnt<< "multi-reads are processed!"<< endl;
  }

  n_mhits = offset; // after reduction, total number of alignments
  
  delete parser;

  if (verbose) cout<< "n_multi = "<< n_multi<< ", n_mhits = "<< n_mhits<< endl<< "processMultiReads is finished."<< endl;
}


/****************************************************************************************************/
// Distribute tasks to differen threads


void distributeTasks() {
  params = new ParamType[num_threads];
  threads = new pthread_t[num_threads];

  int quo, res, left; // quotient, residule
  vector<int> lefts;

  quo = n_mhits / num_threads;
  res = n_mhits % num_threads;
  left = n_mhits;
  lefts.assign(num_threads, quo);
  for (int i = 0; i < num_threads; ++i) {
    left -= lefts[i];
    if (i < res) --left;
    lefts[i] = left;
  }
  
  // distribute multi-mapping reads as evenly as possible
  int cp = 0; // current pointer 
  left = n_mhits;
  for (int i = 0; i < num_threads; ++i) {
    params[i].sp = cp;
    while (cp < n_multi && left > lefts[i]) left -= multis[cp++].s;
    if (cp - 1 > params[i].sp && (left + multis[cp - 1].s - lefts[i]) < (lefts[i] - left)) {
      left += multis[--cp].s;
    }
    else if (params[i].sp == cp && cp < n_multi)
      left -= multis[cp++].s;
    params[i].ep = cp;
  }
  
  // prepare for the MS step
  SiteType site;
  IterType iter, lb, ub; // left bound, right bound: [lb, ub)
  int lb_pos, ub_pos; // the maximum multi-site pos smaller than lb and ub
  int sumc; // sum of unique counts within [lb, ub - 1]
  
  assert(posMap.size() > 0);
  
  lb = posMap.begin(); ub = lb; ++ub;
  lb_pos = -1;
  ub_pos = (lb->second.aligns.size() > 0 ? 0 : -1);
  sumc = lb->second.c;
  
  sites.clear();
  for (iter = posMap.begin(); iter != posMap.end(); ++iter)
    if (iter->second.aligns.size() > 0) {
      site.v = &iter->second;
      
      while (lb != ub && lb->first.cmp(iter->first, w) < -w) {
	sumc -= lb->second.c;
	if (lb->second.aligns.size() > 0) ++lb_pos;
	++lb;
      }

      if (lb == ub) {
	while (lb != iter && lb->first.cmp(iter->first, w) < -w) {
	  if (lb->second.aligns.size() > 0) ++lb_pos;
	  ++lb;
	}

	ub = lb; ++ub;
	ub_pos = lb_pos + (lb->second.aligns.size() > 0 ? 1 : 0);
	sumc = lb->second.c;
      }

      while (ub != posMap.end() && ub->first.cmp(iter->first, w) <= w) {
	sumc += ub->second.c;
	if (ub->second.aligns.size() > 0) ++ub_pos;
	++ub;
      }

      site.uc = sumc;
      site.left = lb_pos + 1;
      site.right = ub_pos;
      sites.push_back(site);
    }
  n_msites = sites.size();
  
  // distribute aligned positions as evenly as possible
  int cs = 0, ps = -1, ns = -1; // current, previous, and next site
  int psum = 0; // partial sum
  left = n_mhits;
  for (int i = 0; i < num_threads; ++i) {
    params[i].ss = cs;
    if (cs == n_msites) { params[i].es = cs; continue; }

    if (ns >= 0) { left -= psum; cs = ns; ns = -1; }
    else {
      while (cs < n_msites && sites[cs].right != cs) left -= sites[cs++].v->aligns.size();
      assert(cs < n_msites);
      left -= sites[cs++].v->aligns.size();
    }
    
    ps = -1;
    while (cs < n_msites && left > lefts[i]) {
      ps = cs; psum = 0;
      while (cs < n_msites && sites[cs].right != cs) psum += sites[cs++].v->aligns.size();
      assert(cs < n_msites);
      psum += sites[cs++].v->aligns.size();
      left -= psum;
    }

    if (ps >= 0 && (left + psum - lefts[i]) < (lefts[i] - left)) {
      left += psum; ns = cs; cs = ps;
    }
    
    params[i].es = cs;
  }
  
  // initialize pthreads
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

  if (verbose) cout<< "distributeTasks is finished."<< endl;
}


/****************************************************************************************************/
// The Expectation-Maximization-Smooth algorithm


// Expectation step
void* E_STEP(void* arg) {
  ParamType *param = (ParamType*)arg;
  double *my_fracs, *my_conprbs;
  double sum;
  
  for (int i = param->sp; i < param->ep; ++i) {
    my_fracs = fracs + multis[i].offset;
    my_conprbs = conprbs + multis[i].offset;
    sum = 0.0;
    for (int j = 0; j < multis[i].s; ++j) {
      my_fracs[j] *= my_conprbs[j];
      sum += my_fracs[j];
    }
    
    //assert(sum > 0.0);
    if (sum <= 0.0) sum = 1.0;
    
    for (int j = 0; j < multis[i].s; ++j)
      my_fracs[j] = my_fracs[j] / sum * multis[i].c;
  }

  return NULL;
}

// Maximization-smooth step
void* MS_STEP(void* arg) {
  ParamType *param = (ParamType*)arg;
  int l, r;
  double psum;

  if (last_round) {
    for (int i = param->ss; i < param->es; ++i)
      sites[i].v->collect();
    return NULL;
  }
  
  l = param->ss; r = param->ss - 1; psum = 0.0;
  for (int i = param->ss; i < param->es; ++i) {
    while (r < sites[i].right) {
      ++r;
      sites[r].v->collect();
      psum += sites[r].v->weight;
    }
    while (l < sites[i].left) {
      psum -= sites[l].v->weight;
      ++l;
    }
    assert(l == sites[i].left && r == sites[i].right);
    if (psum < 0.0) psum = 0.0;
    sites[i].v->push(psum + sites[i].uc);
  }
  
  return NULL;
}

void EMS(int ROUNDS) {
  for (int ROUND = 1; ROUND <= ROUNDS; ++ROUND) {
    // E step
    for (int i = 0; i < num_threads; ++i) {
      rc = pthread_create(&threads[i], &attr, E_STEP, (void*)&params[i]);
      pthread_assert(rc, "pthread_create", "Cannot create thread " + itos(i) + " (numbered from 0) for E_STEP at ROUND " + itos(ROUND) + "!");
    }

    for (int i = 0; i < num_threads; ++i) {
      rc = pthread_join(threads[i], NULL);
      pthread_assert(rc, "pthread_join", "Cannot join thread " + itos(i) + " (numbered from 0) for E_STEP at ROUND " + itos(ROUND) + "!");
    }
    
    // M-S step
    last_round = ROUND == ROUNDS;
    for (int i = 0; i < num_threads; ++i) {
      rc = pthread_create(&threads[i], &attr, MS_STEP, (void*)&params[i]);
      pthread_assert(rc, "pthread_create", "Cannot create thread " + itos(i) + " (numbered from 0) for MS_STEP at ROUND " + itos(ROUND) + "!");
    }

    for (int i = 0; i < num_threads; ++i) {
      rc = pthread_join(threads[i], NULL);
      pthread_assert(rc, "pthread_join", "Cannot join thread " + itos(i) + " (numbered from 0) for MS_STEP at ROUND " + itos(ROUND) + "!");
    }

    if (verbose && ROUND % 10 == 0) cout<< ROUND<< " iterations are done."<< endl;
  }

  if (verbose) cout<< "EMS algorithm is finished."<< endl;
}


/****************************************************************************************************/


void output() {
  sprintf(outF, "%s.site_info", imdName);
  FILE *fo = fopen(outF, "w");

  IterType iter;

  for (iter = posMap.begin(); iter != posMap.end(); ++iter)
    fprintf(fo, "%d %c %d\t%d\t%.2f\n", iter->first.cid, iter->first.dir, iter->first.pos, iter->second.c, iter->second.weight);

  fclose(fo);

  if (verbose) cout<< "output is finished."<< endl;
}


/****************************************************************************************************/
// initialization and final release of resource


void init() {
  model = new PROBerReadModel_iCLIP(model_type, max_len);

  posMap.clear();
  fracs = conprbs = NULL;
}

void release() {
  sprintf(modelF, "%s.model", imdName);
  model->write(modelF);
  delete model;

  delete[] fracs;
  delete[] conprbs;

  delete[] params;
  delete[] threads;
}


/****************************************************************************************************/


int main(int argc, char* argv[]) {
  // n_threads here
  if (argc < 6) { 
    printf("PROBer-analyze-iCLIP model_type imdName alignF w num_threads [-m max_hit_allowed] [--shorter-than min_len] [--keep-alignments] [--max-len max_len] [--rounds rounds] [-q]\n");
    exit(-1);
  }

  model_type = atoi(argv[1]);
  strcpy(imdName, argv[2]);
  w = atoi(argv[4]);
  num_threads = atoi(argv[5]);

  
  bowtie_filter = false;
  max_hit_allowed = 2147483647; // 2^31 - 1
  min_len = -1;
  max_len = -1;
  keep_alignments = false;
  rounds = 100; // default is 100 rounds
  
  for (int i = 6; i < argc; ++i) {
    if (!strcmp(argv[i], "-q")) verbose = false;
    if (!strcmp(argv[i], "-m")) max_hit_allowed = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--shorter-than")) min_len = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--keep-alignments")) keep_alignments = true;
    if (!strcmp(argv[i], "--max-len")) max_len = atoi(argv[i + 1]);
    if (!strcmp(argv[i], "--rounds")) rounds = atoi(argv[i + 1]);
  }

  init();  
  parseAlignments(argv[3]);
  model->finish();
  processMultiReads();
  distributeTasks();
  EMS(rounds);
  output();
  release();

  return 0;
}
