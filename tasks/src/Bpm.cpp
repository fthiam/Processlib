#include "Bpm.h"
#include "GslErrorMgr.h"
//#include <gsl_cw.h>
#include <gsl/gsl_fft_complex.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_multifit.h>
#include <gsl/gsl_spline.h>
#include <iostream>
#include <sys/time.h>
#include <time.h>
#include "PoolThreadMgr.h"

using namespace Tasks;

template<class IN> static inline IN max(IN a,IN b)
{
  return a > b ? a : b;
}
template<class IN> static inline IN min(IN a,IN b)
{
  return a < b ? a : b;
}
/** @brief Calculate the image projections in X and Y and
 *  the integrated pixel intensity on the image.
 */
template<class IN> 
void BpmManager::BpmTask::_treat_image(const Data &aSrc,
				       Buffer &projection_x,Buffer &projection_y,
				       BpmManager::Result &aResult)
{
  unsigned long long *aProjectionX = (unsigned long long*)projection_x.data;
  unsigned long long *aProjectionY = (unsigned long long*)projection_y.data;
  Buffer *aBufferPt = aSrc.buffer;
  IN *aSrcPt = (IN*)aBufferPt->data;
  for(int y = 0;y < aSrc.height;++y)
    for(int x = 0;x < aSrc.width;++x)
      {
	if(*aSrcPt > mThreshold)
	  {
	    aProjectionX[x] += *aSrcPt;
	    aProjectionY[y] += *aSrcPt;
	    aResult.beam_intensity += *aSrcPt;
	  }
	++aSrcPt;
      }
}
/** @brief Calculates the projection X on a limitied area of
 *  the image around the maximum.
 */
template<class IN>
void BpmManager::BpmTask::_tune_projection(const Data &aSrc,
					   Buffer &projection_x,Buffer &projection_y,
					   const BpmManager::Result &aResult)
{
  unsigned long long *aProjectionX = (unsigned long long*)projection_x.data;
  memset(aProjectionX,0,aSrc.width * sizeof(unsigned long long));
  
  unsigned long long *aProjectionY = (unsigned long long*)projection_y.data;
  memset(aProjectionY,0,aSrc.height * sizeof(unsigned long long));

  Buffer *aBufferPt = aSrc.buffer;
  IN *aSrcPt = (IN*)aBufferPt->data;
  
  // X tuning profile
  int tuning_margin = (int)round((aResult.beam_fwhm_y * (mFwhmTunningExtension - 1)) / 2.);
  int fwhm_min = max(mBorderExclusion,aResult.beam_fwhm_min_y_index - tuning_margin);
  int fwhm_max = min(aResult.beam_fwhm_max_y_index + tuning_margin,
		     aSrc.height - 1 - mBorderExclusion);
  for(int y = fwhm_min;y < (fwhm_max + 1);++y)
    {
      IN *aBufferPt = aSrcPt + (y * aSrc.width) + mBorderExclusion;
      for(int x = mBorderExclusion;x < (aSrc.width - mBorderExclusion);++x,++aBufferPt)
	aProjectionX[x] += *aBufferPt;
    }

  // Y tuning profile
  tuning_margin = (int)round((aResult.beam_fwhm_x * (mFwhmTunningExtension - 1)) / 2.);
  fwhm_min = max(mBorderExclusion,aResult.beam_fwhm_min_x_index - tuning_margin);
  fwhm_max = min(aResult.beam_fwhm_max_x_index + tuning_margin,
		 aSrc.width - 1 - mBorderExclusion);
  
  for(int y = mBorderExclusion;y < (aSrc.height - mBorderExclusion);++y)
    {
      IN *aBufferPt = aSrcPt + (y * aSrc.width) + fwhm_min;
      for(int x = fwhm_min;x < (fwhm_max + 1);++x,++aBufferPt)
	aProjectionY[y] += *aBufferPt;
    }
}
/** @brief Find the position where the maximum is.
 *  average over three values and find the maximum in Y
 */
inline int _max_intensity_position(const Buffer &projection,int aSize) 
{
  int aMaxPos = -1;
  unsigned long long aMaxValue = 0;
  const unsigned long long *aProjection = (unsigned long long*)projection.data;

  for(int i = 1;i < (aSize -2);++i)
    {
      unsigned long long aValue = aProjection[i - 1] + aProjection[i] + aProjection[i + 1];
      if(aValue > aMaxValue)
	aMaxValue = aValue,aMaxPos = i;
    }
  return aMaxPos;
}
/** @brief Find the maximum from the x and y projections.
 *  and get the intensity at this point averaged
 *  over 5 pixels.
 *  Normally this should be the position of the beam spot.
 */
template<class IN> 
static void _max_intensity(const Data &aSrc,
			   const Buffer &projection_x,const Buffer & projection_y,
			   BpmManager::Result &aResult)
{
  Buffer *aBufferPt = aSrc.buffer;
  IN *aSrcPt = (IN*)aBufferPt->data;
  
  int yMaxPos = _max_intensity_position(projection_y,aSrc.height);
  int xMaxPos = _max_intensity_position(projection_x,aSrc.width);
  // get the five pixel values around x_max_pos and y_max_pos
  if(xMaxPos > 0 && xMaxPos < (aSrc.width - 1) &&
     yMaxPos > 0 && yMaxPos < (aSrc.height - 1))
    {
      aResult.beam_center_x = (double)xMaxPos;
      aResult.beam_center_y = (double)yMaxPos;
      // Center left Pixel
      aSrcPt += yMaxPos * aSrc.width + xMaxPos - 1;
      unsigned long long aBeamSum = *aSrcPt;
      // Center Pixel
      ++aSrcPt;
      aBeamSum += *aSrcPt;
      // Center right Pixel
      ++aSrcPt;
      aBeamSum += *aSrcPt;
      // Top Pixel
      aSrcPt -= aSrc.width + 1;
      aBeamSum += *aSrcPt;
      // Bottom Pixel
      aSrcPt += aSrc.width << 1;
      aBeamSum += *aSrcPt;
      aResult.beam_intensity = (double)(aBeamSum) / 5.;
    }
}

double BpmManager::BpmTask::_calculate_fwhm(const Buffer &projectionBuffer,int size,
					    int peak_index,double background_level,
					    int &min_index,int &max_index)
{
  const unsigned long long *aProjectionPt = (const unsigned long long*)projectionBuffer.data;
  unsigned long long max_value = aProjectionPt[peak_index];
  double usedBackgroundLevel;
  if(mEnableBackgroundSubstration) 
    usedBackgroundLevel = background_level;
  else
    {
      usedBackgroundLevel = double(aProjectionPt[mBorderExclusion] +
				   aProjectionPt[mBorderExclusion + 1] +
				   aProjectionPt[mBorderExclusion + 2] +
				   aProjectionPt[size - mBorderExclusion - 1] +
				   aProjectionPt[size - mBorderExclusion - 2] +
				   aProjectionPt[size - mBorderExclusion - 3]) / 6.;
    }
  // treat the case of strange data
  if(max_value < (3. + usedBackgroundLevel))
    {
      min_index = max_index = 0;
      return -1.;
    }
  double hm = ((max_value - usedBackgroundLevel) / 2.) + usedBackgroundLevel;
  // FIRST ON THE LEFT SIDE OF THE CURVE
  int index;
  for(index = peak_index;aProjectionPt[index] > hm && index > 0;--index);
  double val_h = aProjectionPt[index - 1] - hm;
  double val_l = aProjectionPt[index] - hm;
  double correction1;
  if(fabs(val_h) < fabs(val_l))
    {
      min_index = index - 1;
      correction1 = -1. * val_h / (val_h - val_l); 
    }
  else
    {
      min_index = index;
      correction1 = val_l / (val_l - val_h);
    }

  // NOW ON THE RIGHT SIDE OF THE CURVE
  for(index = peak_index;aProjectionPt[index] > hm && index < (size - 1);++index);
  val_h = aProjectionPt[index - 1] - hm;
  val_l = aProjectionPt[index] - hm;
  double correction2;
  if(fabs(val_h) < fabs(val_l))
    {
      max_index = index - 1;
      correction2 = -1. * val_h / (val_h - val_l);
    }
  else
    {
      max_index = index;
      correction2 = val_l / (val_l - val_h);
    }
  return max_index - correction2 - min_index - correction1; // fwhm result
}
/** @brief Caluclate the background level around the peak
 */
void BpmManager::BpmTask::_calculate_background(const Buffer &projection,double &background_level,
						int min_index,int max_index)
{
  unsigned long long *aProjectionPt = (unsigned long long*)projection.data;
  if(mEnableBackgroundSubstration)
    background_level = (aProjectionPt[min_index] + aProjectionPt[max_index]) / 2.;
  else
    background_level = 0.;
}
/** @the distribution support is extended from ly to n
 *  the values of y at the (n-ly) new points tend to zero linerarly
 */
static inline void _extend_y(double arr[],int size,
			     double ret_arr[],int ret_size)
{
  double offset = arr[0];
  for(int i = 0;i < size;++i)
    arr[i] -= offset;
  double yend = arr[size - 1];
  double incz = -yend/(ret_size - size - 1);
  memcpy(ret_arr,arr,size * sizeof(double));
  for(int i = size;i < ret_size;++i,yend += incz)
    ret_arr[i] = yend;
}
/** @brief parabolic fit of a data set of datalength length...
 *  the parabola coefficients are returned in X
 */
static inline int _solve(double data[], int datalength, int first, double x[])
{
#ifdef DEBUG
  printf("6a\n");
  printf("n: %d\n",datalength);
#endif
 
  gsl_matrix *X = gsl_matrix_alloc (datalength, 3);
  gsl_vector *y = gsl_vector_alloc (datalength);
  
  int error_flag = GslErrorMgr::get().lastErrno();
#ifdef DEBUG
  printf("error_flag: %d\n",error_flag);
#endif
  if (error_flag) return 1;

#ifdef DEBUG
  printf("6b\n");
#endif

  gsl_vector *c = gsl_vector_alloc(3);
  gsl_matrix *cov = gsl_matrix_alloc (3, 3);
  for (int i = 0; i < datalength;++i)
    {
      gsl_matrix_set (X, i, 0, 1.0);
      gsl_matrix_set (X, i, 1, double(i+first));
      gsl_matrix_set (X, i, 2, double((i+first)*(i+first)));
      gsl_vector_set (y, i, data[i]);
    }
  gsl_multifit_linear_workspace *work = gsl_multifit_linear_alloc (datalength, 3);
  double chisq;
  gsl_multifit_linear (X, y, c, cov, &chisq, work);
  gsl_multifit_linear_free (work);

  for(int i = 0;i < 3;++i)
    x[i] = gsl_vector_get(c,i);

  gsl_matrix_free(X);
  gsl_vector_free(y);
  gsl_matrix_free(cov);
  gsl_vector_free(c);

  return 0;
}

//find first poind and width of Y distribution such as y>threshold
static void _find_summit(double arr[],int size,int &first_point,int &nb_points)
{
  static const double THRESHOLD = 0.9;
  double max = arr[0];

  for(int i = 0;i < size;++i)
    if(arr[i]>max) max = arr[i];
 
  double threshold = max * THRESHOLD;
  nb_points = 0;
  for(int i = 0;i < size;i++)
    if(arr[i] > threshold)
      {
	if(!nb_points) first_point = i;
	++nb_points;
      }
}
#define REAL(z,i) ((z)[i << 1])	// i << 1 == 2i
#define IMAG(z,i) ((z)[(i << 1) + 1])

/** @brief find a center of a gaussian curve
 */
static inline double _compute_center(double y[],int ly)
{
  double result = -1.0;
  int n;
  for(n = 1;n < ly;n <<= 1);	// Find the next above power of two
  int n1 = n << 2;
  
  GslErrorMgr::get().resetErrorMsg();

  Buffer *ynRe = new Buffer(n * sizeof(double));
  double *yn_re = (double*)ynRe->data;
  // y is extended from ly to n to give yn
  // the values of yn at the (n-ly) new points tend to zero linerarly
  // function not checked for failure as not much can go wrong
  _extend_y(y,ly,yn_re,n);

  Buffer *dataN = new Buffer(n * 2 * sizeof(double));	// REAL + IMAG in double
  double *data_n = (double*)dataN->data;
  //fn =FFT(yn)
  for(int i = 0;i < n;++i)
    {
      REAL(data_n,i) = yn_re[i];
      IMAG(data_n,i) = 0.0;
    }

  int error_flag = gsl_fft_complex_radix2_forward(data_n,1,n);

  Buffer *dataN1 = new Buffer(2 * n1 * sizeof(double));
  double *data_n1 = (double*)dataN1->data;
  Buffer *yn1Re = new Buffer(n1 * sizeof(double));
  double *yn1_re = (double*)yn1Re->data;
  if (!error_flag)
    {
      // f is extended from n to n1 to give fn1
      // (fn is split in two and zero-padded in the middle)
      memset(data_n1,0,n1 * sizeof(double));
      // copy first half of data_n at the beginnig of data_n1
      memcpy(data_n1,data_n,n * sizeof(double));
      // copy second half of data_n at the end of data_n1
      memcpy(data_n1 + (n1 - n / 2),data_n + n,n * sizeof(double));
      // fn1 is squared
      for(int i = 0;i < n1;++i)
	{
	  double rl = REAL(data_n1,i);
	  double img = IMAG(data_n1,i);
	  REAL(data_n1,i) = rl*rl-img*img;
	  IMAG(data_n1,i)=2*rl*img;
	}
      //yn1=IFFT(fn1)...
      error_flag = gsl_fft_complex_radix2_inverse (data_n1, 1, n1);
#ifdef DEBUG
      printf("ifft done..\n");
#endif
       
      //absolute value of yn1
      if (!error_flag) 
	{
	  for (int i = 0;i < n1;++i)
	    {
	      double rl = REAL(data_n1,i);
	      double img = IMAG(data_n1,i);
	      yn1_re[i] = sqrt(rl*rl+img*img);
	    }

	  //find  point and width of yn1 such as yn1>threshold
	  int first_point,nb_points;
	  _find_summit(yn1_re,n1,first_point,nb_points);
#ifdef DEBUG
	  printf("summit done... nb_points = %d\n", nb_points);
#endif
	  if (!nb_points)
	    error_flag = 1;
	  else  //'final' distribution
	    {
#ifdef DEBUG
	      printf("final distribution done..\n");
#endif
	      // given the summit 'final' distribution,
	      // parabolic fit and computation of parabola summit position
	      double X[3];
	      error_flag = _solve(yn1_re + first_point,
				  nb_points, first_point, X);
#ifdef DEBUG
	      printf("solve done..\n");
#endif
	      if (!error_flag)
		result = -X[1]/X[2]/2.0/8.0;
	      else 
		{
#ifdef DEBUG
		  printf("ERROR! result = -1.0\n");
#endif
		  std::cerr << GslErrorMgr::get().lastErrorMsg() << std::endl;
		  GslErrorMgr::get().resetErrorMsg();
		}
	    }
	}
    }

  //Free allocated variable
  ynRe->unref();
  dataN->unref();
  dataN1->unref();
  yn1Re->unref();
  return result;
}
//@brief constructor
BpmManager::BpmTask::BpmTask(BpmManager &aMgr) :
  mFwhmTunning(false),
  mFwhmTunningExtension(1.5),
  mAoiExtension(4.),
  mBorderExclusion(10),
//   mGaussFittMax(false),
//   mAverage(1),
  mThreshold(0),
  mEnableX(true),
  mEnableY(true),
  mEnableBackgroundSubstration(false),
  mRoiAutomatic(true),
  _mgr(aMgr)
{
  //Init Roi
  //  mRoi = {-1,-1,-1,-1};
}
BpmManager::BpmTask::BpmTask(const BpmManager::BpmTask::BpmTask &aTask) :
  Task(aTask),
  mFwhmTunning(aTask.mFwhmTunning),
  mFwhmTunningExtension(aTask.mFwhmTunningExtension),
  mAoiExtension(aTask.mAoiExtension),
  mBorderExclusion(aTask.mBorderExclusion),
  //   mGaussFittMax(aTask.mGaussFittMax),
  //   mAverage(aTask.mAverage),
  mThreshold(aTask.mThreshold),
  mEnableX(aTask.mEnableX),
  mEnableY(aTask.mEnableY),
  mEnableBackgroundSubstration(aTask.mEnableBackgroundSubstration),
  mRoiAutomatic(aTask.mRoiAutomatic),
  _mgr(aTask._mgr)
{
  
}

Task* BpmManager::BpmTask::copy() const
{
  return new BpmTask(*this);
}

#define COMPUTE_BEAM_POSITION(XorY,WidthorHeight) \
{ \
  int min_index,max_index; \
  aResult.beam_fwhm_##XorY = _calculate_fwhm(*projection_##XorY,WidthorHeight , \
					     int(aResult.beam_center_##XorY), \
					     mBackgroundLevel##XorY,	\
					     min_index,max_index); \
  /* check for strange results */ \
  if(max_index <= min_index || max_index <= 0 || aResult.beam_fwhm_##XorY <= 0.) \
    { \
      aResult.beam_fwhm_##XorY = -1.; \
      aResult.beam_center_##XorY = -1.; \
      mBackgroundLevel##XorY = 0; \
      /*@todo maybe register in BpmManager::Result with enum that call failed */ \
    } \
  else \
    { \
      aResult.beam_fwhm_min_##XorY##_index = min_index; \
      aResult.beam_fwhm_max_##XorY##_index = max_index; \
      if(mRoiAutomatic) \
	{ \
	  int AOI_margin = (int)round((aResult.beam_fwhm_##XorY *(mAoiExtension - 1)) / 2.); \
	  min_index = max(mBorderExclusion,min_index - AOI_margin); \
	  max_index = min(max_index + AOI_margin,WidthorHeight - 1 - mBorderExclusion); \
	  aResult.AOI_min_##XorY = min_index; \
	  aResult.AOI_max_##XorY = max_index; \
	} \
      else \
	{ \
	  aResult.AOI_automatic = false; \
	  /* @todo No Roi Management for now maybe*/	\
	  min_index = 0; \
	  max_index = WidthorHeight; \
	} \
      _calculate_background(*projection_##XorY,mBackgroundLevel##XorY, \
			    min_index,max_index);		      \
      /* calculate the beam center */				      \
      int size = max_index - min_index + 1; \
      Buffer *profile = new Buffer(size * sizeof(double)); \
      double *aProfilePt = (double*)profile->data; \
      unsigned long long *aSrcProfilePt = (unsigned long long*)projection_##XorY->data; \
      for(int i = 0;i < size;++i) /* @todo optimized if needed */	\
	aProfilePt[i] = double(aSrcProfilePt[i]); \
	       \
      aResult.beam_center_##XorY = _compute_center(aProfilePt,size) + min_index; \
      profile->unref();	/* free */					\
	       \
      /*if(aResult.beam_center_x <= 0) @todo should manage error	\
       error = "Beam center calculation failed" \
       error += gsl_cw_error_message();*/	\
    } \
}

#define TUNE_FWHM(XorY,WidthorHeight)  \
{ \
  int min_index,max_index; \
  aResult.beam_fwhm_##XorY = _calculate_fwhm(*projection_##XorY,WidthorHeight, \
					_max_intensity_position(*projection_##XorY,WidthorHeight), \
					mBackgroundLevelTune##XorY, \
					min_index,max_index); \
 \
  int AOI_margin = (int)round((aResult.beam_fwhm_##XorY * (mAoiExtension - 1)) / 2.); \
  min_index = max(mBorderExclusion,min_index - AOI_margin); \
  max_index = min(max_index + AOI_margin,WidthorHeight - 1 - mBorderExclusion); \
	       \
  _calculate_background(*projection_##XorY,mBackgroundLevelTune##XorY, \
			min_index,max_index); \
}

#define PROCESS(TYPE) \
{				\
  _treat_image<TYPE>(aSrc,*projection_x,*projection_y,aResult);  \
  _max_intensity<TYPE>(aSrc,*projection_x,*projection_y,aResult); \
if(mEnableX || mFwhmTunning) \
  COMPUTE_BEAM_POSITION(x,aSrc.width); \
if(mEnableY || mFwhmTunning) \
  COMPUTE_BEAM_POSITION(y,aSrc.height); \
if(mFwhmTunning) \
  { \
    if(aResult.beam_fwhm_x > 0. && aResult.beam_fwhm_y > 0.) \
      { \
	_tune_projection<TYPE>(aSrc,*projection_x,*projection_y,aResult); \
	TUNE_FWHM(x,aSrc.width); \
	TUNE_FWHM(y,aSrc.height); \
      } \
  } \
}
Data BpmManager::BpmTask::process(Data &aSrc)
{

  BpmManager::Result aResult;
  _mgr._getLastBackgroundLevel(*this);
  aResult.frameNumber = aSrc.frameNumber;
  int aSize = sizeof(unsigned long long) * aSrc.width;
  Buffer *projection_x = new Buffer(aSize);
  memset (projection_x->data, 0,aSize);
  
  aSize = sizeof(unsigned long long) * aSrc.height;
  Buffer *projection_y = new Buffer(aSize);
  memset (projection_y->data, 0,aSize);
  switch(aSrc.depth())
    {
    case 1: PROCESS(unsigned char);break;
    case 2: PROCESS(unsigned short);break;
    case 4: PROCESS(unsigned int);break;
    default:
      // should throw an error
      std::cerr << "BpmManager : Data depth of " << aSrc.depth() << "not implemented " << std::endl;
      goto clean;
    }
 
 clean:
  projection_x->unref();
  projection_y->unref();
  _mgr._setResult(aResult,*this);
  return Data();
}


/** @brief BpmManager class container of the result of the Bpm calculation
 */
BpmManager::BpmManager(int historySize) :
  _lastBackgroundLevelx(0.),
  _lastBackgroundLevely(0.),
  _lastBackgroundLevelTunex(0.),
  _lastBackgroundLevelTuney(0.),
  _currentFrameNumber(0)
{
  pthread_mutex_init(&_lock,NULL);
  pthread_cond_init(&_cond,NULL);
  _historyResult.resize(historySize);
}

BpmManager::~BpmManager()
{
  pthread_mutex_destroy(&_lock);
  pthread_cond_destroy(&_cond);
}
/** @brief get a BpmTask.
 *  This methode create a BpmTask linked to this BpmManger
 */
BpmManager::BpmTask* BpmManager::getBpmTask()
{
  return new BpmManager::BpmTask(*this);
}
/** @brief get the Result class of the Bpm calculation.
    @param timeout the maximum wait to get the result, timeout is in second
    @param frameNumber the frame id you want or last frame if < 0
*/
BpmManager::Result BpmManager::getResult(double askedTimeout,
					 int frameNumber) const
{
  if(askedTimeout >= 0.)
    {
      struct timeval now;
      struct timespec timeout;
      int retcode = 0;
      gettimeofday(&now,NULL);
      timeout.tv_sec = now.tv_sec + long(askedTimeout);
      timeout.tv_nsec = (now.tv_usec * 1000) + 
	long((askedTimeout - long(askedTimeout)) * 1e9);
      PoolThreadMgr::Lock aLock(&_lock);
      while(!_isFrameAvailable(frameNumber) && retcode != ETIMEDOUT)
	retcode = pthread_cond_timedwait(&_cond,&_lock,&timeout);
      if(retcode == ETIMEDOUT)
	return BpmManager::Result(BpmManager::Result::TIMEDOUT);
    }
  else
    {
      PoolThreadMgr::Lock aLock(&_lock);
      while(!_isFrameAvailable(frameNumber))
	pthread_cond_wait(&_cond,&_lock);
    }

  if(frameNumber < 0)
    frameNumber = _currentFrameNumber;
  else if(frameNumber < (_currentFrameNumber - int(_historyResult.size())))
    return BpmManager::Result(BpmManager::Result::NO_MORE_AVAILABLE);

  // still in history
  int aResultPos = frameNumber % _historyResult.size();
  return _historyResult[aResultPos];
}


/** @brief return the Result history
 */
void BpmManager::getHistory(std::list<BpmManager::Result> & anHistory) const
{
  anHistory.clear();
  PoolThreadMgr::Lock aLock(&_lock);
  for(std::vector<Result>::const_iterator i = _historyResult.begin();
      i != _historyResult.end();++i)
    {
      if(i->frameNumber >= 0)
	anHistory.push_back(*i);
    }
  anHistory.sort();
}
/** @brief set the number of result keeped in history.
 *  It resize the result history and clear it
 */
void BpmManager::resizeHistory(int aSize)
{
  if(aSize < 1) aSize = 1;
  PoolThreadMgr::Lock aLock(&_lock);
  _historyResult.resize(aSize);
  // set all result to invalide state
  for(std::vector<Result>::iterator i = _historyResult.begin();
      i != _historyResult.end();++i)
    i->frameNumber = -1;	
  _currentFrameNumber = 0;
}
/** @brief check if the frame asked is available.
 *  check if at the position in the history the result 
 *  have a frame number >= 0 and if so return true
 *  @warning All this call must be under mutex lock
 *  @param frameNumber the frameNumber or < 0 if last
 */
bool BpmManager::_isFrameAvailable(int aFrameNumber) const
{
  if(aFrameNumber < 0) aFrameNumber = _currentFrameNumber;

  if(aFrameNumber < (_currentFrameNumber - int(_historyResult.size()))) 
    return true;		// the frame asked is no more available
  else
    {
      int aResultPos = aFrameNumber % _historyResult.size();
      return _historyResult[aResultPos].frameNumber >= 0;
    }
}

/** @brief copy all background level from last calculate
 */
void BpmManager::_getLastBackgroundLevel(BpmManager::BpmTask &aTask) const
{
  PoolThreadMgr::Lock aLock(&_lock);
  aTask.mBackgroundLevelx = _lastBackgroundLevelx;
  aTask.mBackgroundLevely = _lastBackgroundLevely;
  aTask.mBackgroundLevelTunex = _lastBackgroundLevelTunex;
  aTask.mBackgroundLevelTuney = _lastBackgroundLevelTuney;
}
/** @brief set a new result and synchronise other thread waiting for it.
 */
void BpmManager::_setResult(const BpmManager::Result &aResult,
			    const BpmManager::BpmTask &aTask)
{
  PoolThreadMgr::Lock aLock(&_lock);
  if(aResult.frameNumber > _currentFrameNumber)
    {
      _currentFrameNumber = aResult.frameNumber;
      //copy Background Level
      _lastBackgroundLevelx = aTask.mBackgroundLevelx;
      _lastBackgroundLevely = aTask.mBackgroundLevely;
      _lastBackgroundLevelTunex = aTask.mBackgroundLevelTunex;
      _lastBackgroundLevelTuney = aTask.mBackgroundLevelTuney;
    }
  int aResultPos = aResult.frameNumber % _historyResult.size();
  if(aResultPos < 0) aResultPos = 0;
  _historyResult[aResultPos] = aResult;
  pthread_cond_broadcast(&_cond);
}
