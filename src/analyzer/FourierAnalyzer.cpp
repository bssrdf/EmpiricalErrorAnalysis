#include <FourierAnalyzer.h>
#include <cmdlnparser.h>
#include <common.h>
#include <fstream>
#include <iomanip>
#include <write-exr.h>

#include <tbb/tbb.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_scheduler_init.h>

///
/// \brief FourierAnalyzer::trialStepStr
///

const string FourierAnalyzer::trialStepStr = "--tstep" ;
const string FourierAnalyzer::freqStepStr = "--wstep" ;

////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Fourier Power spectrum computation using continuous Fourier Transform
// Parallelization is done using OpenMP
////////////////////////////////////////////////////////////////////////////////////////////////////////////

FourierAnalyzer::~FourierAnalyzer(){
    delete [] _powerSpectrum;
}

Analyzer* FourierAnalyzer::createAnalyzer(Sampler *s, Integrand* I, const vector<string>& AnalyzerParams){
    return new FourierAnalyzer(s, AnalyzerParams);
}

FourierAnalyzer::FourierAnalyzer(Sampler* s, const vector<string>& AnalyzerParams)
    : _trialStepOut(1), _frequencyStep(1.0) {

    AnalyzerType = "fourier";
    _sampler = s;
    CLParser::FindMultiArgs<int>(-1, _nSamples, AnalyzerParams, NSampStr) ;
    _nTrials = CLParser::FindArgument<int>(AnalyzerParams, nTrialsStr) ;
    _trialStepOut = CLParser::FindArgument<int>(AnalyzerParams, trialStepStr) ;
    _frequencyStep = CLParser::FindArgument<float>(AnalyzerParams, freqStepStr) ;

    _xRes = 512;
    _yRes = 512;
    _powerSpectrum = new float[_xRes * _yRes]();
    _complexSpectrum = new std::complex<float>[_xRes*_yRes]();

    //    std::cerr << _nSamples << " " << _shear << " "<< _nTrials<< std::endl;
}

void FourierAnalyzer::continuous_fourier_spectrum(){

    int half_xRes = _xRes * 0.5;
    int half_yRes = _yRes * 0.5;
    int npoints = _pts.size();

    //tbb::tick_count t0 = tbb::tick_count::now();
    //tbb::task_scheduler_init init(8);
    tbb::parallel_for(
                tbb::blocked_range2d<int>(0,_xRes, 16, 0, _yRes, 16),
                [=](const tbb::blocked_range2d<int>& imgblock ) {
        for( int row = imgblock.rows().begin(); row != imgblock.rows().end(); ++row ){
            for( int col = imgblock.cols().begin(); col != imgblock.cols().end(); ++col ) {
                double fx = 0.f, fy = 0.f;
                double wx = (col-half_xRes)*_frequencyStep;;
                double wy = (row-half_yRes)*_frequencyStep;;

                for (int i = 0; i < npoints; ++i) {
                    double exp = -twopi * (wx * _pts[i].x + wy * _pts[i].y);
                    fx += cos(exp);
                    fy += sin(exp);
                }

                _complexSpectrum[row*_xRes+col].real(fx); ///real part
                _complexSpectrum[row*_xRes+col].imag(fy);  ///imaginary part
            }
        }
    }
    );

}

void FourierAnalyzer::power_fourier_spectrum(){

    for(int r = 0; r < _yRes; r++){
        for(int c = 0; c < _xRes; c++){
            double real = _complexSpectrum[(r*_xRes+c)].real();
            double imag = _complexSpectrum[(r*_xRes+c)].imag();

            double powerVal = (real*real + imag*imag) / (_pts.size());
            _powerSpectrum[(r*_xRes+c)] = powerVal;
        }
    }
}

void FourierAnalyzer::compute_radial_mean_powerspectrum(const std::string& filename){


    if(_xRes != _yRes){
        std::cerr << "We assume square images for radial mean power computation !!!" << std::endl;
        exit(-2);
    }
    ///Radial Power spectrum
    int halfwidth = _xRes*0.5;

    double* radialHistogram = new double[halfwidth]();
    int* histoCounter = new int[halfwidth]();
    int xcenter = halfwidth;
    int ycenter = halfwidth;
    for(int r = 0; r < _yRes; r++){
        for(int c = 0; c < _xRes; c++){
            double dx = xcenter-c;
            double dy = ycenter-r;
            double distance = sqrt(dx*dx+dy*dy);

            int imgIndex = r*_xRes+c;
            int index = distance;
            if(distance > halfwidth-1)
                continue;
            else{
                radialHistogram[index] += _powerSpectrum[imgIndex];
                histoCounter[index] += 1;
            }
        }
    }

    for(int i = 0; i < halfwidth; i++)
        radialHistogram[i] /= double(histoCounter[i]);

    std::ofstream file;
    file.open(filename.c_str());

    for(int i = 0; i < halfwidth-5; i++)
        file << i << " " << std::fixed << std::setprecision(15) <<  radialHistogram[i] << std::endl;
    file.close();

    delete [] histoCounter;
    delete [] radialHistogram;
}


void FourierAnalyzer::RunAnalysis(string& prefix){

    ///
    /// \brief powerAccum : Accumulate power over all the realizations (trials)
    ///
    float* powerAccum = new float[_xRes * _yRes]();

    for(int j = 0; j < _nSamples.size(); j++){

        const int n(_nSamples[j]) ;

        for(int trial = 1; trial <= _nTrials; trial++){

            _pts.resize(0);
            _sampler->Sample(_pts, n);

           fprintf(stderr, "\r %d / %d : %d", trial, _nTrials, n);

            for(int i=0; i<_xRes*_yRes; i++)
                _powerSpectrum[i] = 0.;

            continuous_fourier_spectrum();
            power_fourier_spectrum();

            for(int r=0; r < _yRes; r++){
                for(int c=0; c < _xRes; c++){
                    int index = r*_xRes+c;
                    powerAccum[index] += _powerSpectrum[index];
                }
            }

            ///
            /// Output final power in _powerSpectrum as an average over nTrials
            /// after each _trialStepOut steps.
            ///
            if(trial == 1 || trial % _trialStepOut == 0){
                for(int r=0; r < _yRes; r++){
                    for(int c=0; c < _xRes; c++){
                        int index = r*_xRes+c;
                        _powerSpectrum[index] = powerAccum[index] / float(trial);
                    }
                }

                ///
                /// Add index of each file with trailing zeros
                ///
                //##########################################################
                std::stringstream ss;
                ss.str(std::string());
                ss << trial;
                std::string s1 = ss.str();
                paddedzerosN(s1, _nTrials);
                //##########################################################
                ss.str(std::string());
                ss << "power-" << _sampler->GetType() << "-n" << n << "-" << s1 << ".exr";
                write_exr_grey(ss.str(), _powerSpectrum, _xRes, _yRes);

                ss.str(std::string());
                ss << "power-radial-mean-" << _sampler->GetType() << "-n" << n << "-" << s1 << ".txt";
                compute_radial_mean_powerspectrum(ss.str());
            }
        }
    }
    delete [] powerAccum;
    powerAccum = 0;
}



