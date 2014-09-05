#ifndef DMSTRANSMODEL_H_
#define DMSTRANSMODEL_H_

#include<cmath>
#include<cassert>
#include<algorithm>
#include<string>
#include<fstream>

#include "sampling.hpp"

/*
  The coordinate system used outside is 0-based, starting from 5' end.
  The coordinate system used internally is 1-based, starting from 5' end:
     5'               3'
     0 - 1 2 ... n - n+1
   n = transcript length - primer length
 */
class DMSTransModel {
public:
  /*
    @param   learning    if we need to learn parameters
    @param   transcript_length  the length of this transcript
   */
  DMSTransModel(bool learning, int transcript_length = -1, Sampler *sampler = NULL);

  /*
    @param   o   the DMSTransModel object to copy from
    @comment: copy constructor
   */
  DMSTransModel(const DMSTransModel& o);

  ~DMSTransModel();

  /*
    @comment: This function sets parameters shared by all transcripts, should be called before any DMSTransModel object is created.
   */
  static void setGlobalParams(int primer_length, int min_frag_len, int max_frag_len);

  /*
    @return   primer length
   */
  static int get_primer_length() { return primer_length; }

  /*
    @return   minimum fragment length
   */
  static int get_minimum_fragment_length() { return min_frag_len + primer_length; }

  /*
    @return   maximum fragment length
   */
  static int get_maximum_fragment_length() { return max_frag_len + primer_length; }

  /*
    @param   pos     leftmost position in 5' end, 0-based  
    @return   the probability of generating a SE read end at pos
   */
  double getProb(int pos) {
    int start_pos = pos + min_frag_len;
    if (start_pos > len || pos < 0) return 0.0;
    double res = delta * margin_prob[pos] * exp(logsum[start_pos] - logsum[pos]);
    if (pos > 0) res *= (beta == NULL ? gamma[pos] : (gamma[pos] + beta[pos] - gamma[pos] * beta[pos]));

    res /= prob_pass;

    return res;
  }
  
  /*
    @param   pos      same as the above function
    @param   fragment_length     fragment length of the PE read
    @return   the probability of generating a PE read pair end at pos and has fragment length fragment_length
   */
  double getProb(int pos, int fragment_length) {
    int start_pos = pos + fragment_length - primer_length;
    if (start_pos > len || pos < 0) return 0.0;
    double res = delta * exp(logsum[start_pos] - logsum[pos]);
    if (pos > 0) res *= (beta == NULL ? gamma[pos] : (gamma[pos] + beta[pos] - gamma[pos] * beta[pos]));
    
    res /= prob_pass;

    return res;
  }

  /*
    @param   pos       leftmost position in 5' end, 0-based
    @param   frac      the fractional weight of this read
    @comment: This function is for single-end reads
   */
  void update(int pos, double frac) {
    if (pos + min_frag_len > len || pos < 0) return;
    end[pos] += frac;
    isSE = true;
  }

  /*
    @param   pos   same as the above function
    @param   fragment_length    the estimated fragment length according to the two mates
    @param   frac      same as the above function
    @comment: This function is for paired-end reads
   */
  void update(int pos, int fragment_length, double frac) {
    if (pos + fragment_length - primer_length > len || pos < 0) return;
    end[pos] += frac;
    start[pos + fragment_length - primer_length] += frac; 
  }

  /*
    @comment: This function calculate logsum and margin_prob and prob_pass, which are used to speed up the calculation
    @comment: It should be called before getProb or EM is called
   */
  void calcAuxiliaryArrays();

  /*
    @return the probability of passing the size selection step
   */
  double getProbPass() const { return prob_pass; }

  /*
    @comment: set start and end to 0
   */
  void init();

  /*
    @param   N_obs   expected observed counts for this transcript
    @param   round   number of EM rounds to go through
    @comment: Run EM algorithm on a single transcript
   */
  void EM(double N_obs, int round = 1);

  /*
    @param   fin   input stream
    @param   sampler  if not NULL, used for initialize beta vector
    @format:  len [beta/gamma] ... 
   */
  void read(std::ifstream& fin, Sampler* sampler = NULL);

  /*
    @param   fout output stream
    @format: the same as read
   */
  void write(std::ofstream& fout);

  /*
    @param   fout output stream
    @format:  c(rate of being marked) len thetas
   */
  void writeTheta(std::ofstream& fout);

  /*
    @param   sampler  a sampler used for sampling
    @param   pos      sampled 5' position (0-based)
    @param   fragment_length   sampled fragment length, with primer length considered
   */
  void simulate(Sampler* sampler, int& pos, int& fragment_length);

  int getLen() { return len; }
  double* getGamma() { return gamma; }
  double* getBeta() { return beta; }

private:
  static const double eps; // Epsilon used as an allowance on floating point error
  static const double INF; // Define exp(1000) as infinite to avoid the partial sum be -inf

  static int primer_length; // primer_length, the length of primers
  static int min_frag_len, max_frag_len; // min_frag_len and max_frag_len, the min and max fragment length (primer length excluded)

  bool learning; // if learn parameters
  bool isSE; // if reads are SE reads 
  int len; // len, number of position can learn parameters, transcript_length - primer_length
  int efflen; // efflen, number of positions can generate a valid fragment, len - min_frag_len + 1
  double delta; // probability of priming from a particular position, delta = 1.0 / (len + 1)
  double prob_pass; // probability of generating a read that passes the size selection step
  double *gamma, *beta; // gamma, the vector of probability of drop-off at i (1-based); beta, the vector of probability of demtheylation at position i (1-based); 
  double *start, *end; // start, number of reads with first base after primer starting at a position; end, number of reads whose TF drops off at a position

  /*
    comment: Auxiliary arrays below
   */
  double *logsum; // logsum[i] = \sigma_{j=1}^{i} log(1-gamma[j]) if beta == NULL or \sigma_{j=1}^{i} log(1-gamma[j])(1-beta[j]). Thus a product from a to b is exp(logsum[b]-logsum[a-1]). 
  double *margin_prob; // For SE reads, margin_prob[i] = \sigma_{j = i + min_frag_len} ^ {i + max_frag_len} \prod_{k=i + min_frag_len + 1} ^{j} (1 - gamma[k]) * (beta == NULL ? 1.0 : (1 - beta[k]))

  double *start2, *end2; // including hidden data, can be shared by a whole thread of transcripts
};

#endif
