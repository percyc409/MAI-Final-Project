
                                       

string name="Time Aligner";
string description="Detects phase shift and realigns input signals";

//INPUT PARAMETERS - switches
array<double> inputParameters(3);
array<string> inputParametersNames = { "Detection", "Mono", "Align"}; // Switch Names
array<int> inputParametersSteps = {2, 2, 2};
array<string> inputParametersEnums = {"Play;Detect", "Off;Mono", "Off;Align"}; // which units to use
array<double> inputParametersMin = { 0, 0, 0}; // min values
array<double> inputParametersMax = { 1, 1, 1}; // max values
array<double> inputParametersDefault = { 0, 0, 0}; // default values

// OUTPUT STRINGS
array<int> outputStringsMaxLengths = { 100 };
array<string> outputStringsNames = { "Phase", "Polarity" };
array<string>  outputStrings(outputStringsNames.length);

// Index for writing and reading from circular buffer
int writeIndex = 0;
int currentIndex;
int readIndex;

// buffer parameters
array<array<double>> buffers(audioInputsCount);
int bufferSize = 2048;
int mask;

int shift; // detected sample delay

double mono; // monophonic sample value

//Flags for switches
bool makeMono;
bool alignFlag;
bool detectP;

// Detector Class
Detector detector;
const uint phaseAlignBufferSize = uint(hzToSamples(50.));//Detector buffer size
enum DetectorState { Off, WaitForCollect, Collecting, Detecting};//Detector state

double hzToSamples(double hz) { return sampleRate / hz; }


bool initialize() {
	// if not a stereo channel print error message and deactivate plugin
	if (audioInputsCount != 2) {
		print("Stereo channels only!"); // print to log file
		return false; // stops processing a script
	}

	else {
		// create buffer with 2^x size
		int lengthPow2 = KittyDSP::Utils::nextPowerOfTwo(bufferSize);
		mask = lengthPow2;
		for (uint channel = 0, count = buffers.length;channel < count;channel++)
		{
			buffers[channel].resize(lengthPow2);
		}

		return true; // proceeds with processing a script
	}

}

void reset()
{
	// reset buffers with silence
	for (uint channel = 0, count = buffers.length;channel < count;channel++)
	{
		// init with zeros
		for (uint i = 0;i < buffers[channel].length;i++)
		{
			buffers[channel][i] = 0;
		}
	}
	writeIndex = 0;
}

void processBlock(BlockData& data)
{

	if (detectP)
		detector.processBlock(data); //call to detector 

	if (alignFlag)
		shift = detector.sampleDelay; // setting shift to detected delay between channels
	else
		shift = 0;


	for (uint i = 0; i < data.samples[0].length; i++) // looping through each sample in the block
	{
		currentIndex = (writeIndex + i) % mask;

		for (uint channel = 0; channel < audioInputsCount; channel++) //looping through the 2 input channels in block
		{
			//Write to buffer
			
			buffers[channel][currentIndex] = data.samples[channel][i];

			//Read From buffer
			if (shift > 0 and channel == 0) { //Shifting first channel backwards in time if shift is > 0
				readIndex = (currentIndex + shift + mask) % mask;
				data.samples[channel][i] = buffers[channel][readIndex];
			}

			if (channel == 1) {

				if (shift < 0) { //Shifting second channel backwards in time if shift is < 0
					readIndex = (currentIndex - shift + mask) % mask;
					data.samples[channel][i] = buffers[channel][readIndex];
				}

				if (makeMono) {
					//Combine left and right channels to Mono
					mono = (data.samples[0][i] + data.samples[1][i]) / 2;
					data.samples[0][i] = mono;
					data.samples[1][i] = mono;
				}
			}
		}
	}
	

	writeIndex = (writeIndex + data.samples[1].length) % mask;
}



void updateInputParameters() // called whenever an input parameter is changed
{

	makeMono =  inputParameters[1] > .5; // sets mono flag for process block
	alignFlag = inputParameters[2] > .5; // sets align flag for process block

	bool newDetectP = inputParameters[0] > .5;
	if (detectP != newDetectP)
	{
		detectP = newDetectP;
		if (detectP)
			detector.initDetection(); // calls initalising method for detector to prepare for new delay detection
	}

}



class Detector
{
	array<array<double>> buffer(audioInputsCount);
	int state;
	uint wHead;
	int sampleDelay;


	Detector() // constructor initialises buffers
	{
		for (uint ch = 0; ch < buffer.length; ++ch)
			buffer[ch].length = phaseAlignBufferSize;
		wHead = 0;
		state = DetectorState::Off;
		sampleDelay = 0;
	}

	void initDetection() // this is called before a new detection starts to prepare the Detector-object's state.
	{
		if (state != DetectorState::Off) return;
		state = DetectorState::WaitForCollect;
		wHead = 0;
	}

	void processBlock(BlockData& data)// this method is where the detector is called from the process block. It checks the state variable and then decides what method to call
	{
		switch (state)
		{
		case DetectorState::WaitForCollect: WaitForCollect(data); break;
		case DetectorState::Collecting: Collect(data); break;
		default: return;
		}
	}

	void WaitForCollect(BlockData& data) // Sets state to collecting and calls collecting method
	{
		state = DetectorState::Collecting;
		Collect(data, 0);
	}

	void Collect(BlockData& data, uint startSample = 0) // Here the samples of the process block are loaded into the detectors buffer
	{
		for (uint s = startSample; s < data.samplesToProcess; ++s)
		{
			if (wHead < phaseAlignBufferSize)
			{
				for (uint ch = 0; ch < buffer.length; ++ch)
					buffer[ch][wHead] = data.samples[ch][s];
				++wHead;
			}
			else // when the buffer is full the detect method is called
			{
				state = DetectorState::Detecting;
				
				Detect();
				return;
			}
		}
	}

	void Detect() // cross correlation is performed and the delay between input channels derived. 
	{
		int bufferSize = int(phaseAlignBufferSize);

		array<double> correlation(phaseAlignBufferSize); // cross correlation
		for (uint i = 0; i < phaseAlignBufferSize; ++i)
		{
			correlation[i] = 0.;
			for (uint j = 0; j < phaseAlignBufferSize; ++j)
			{
				uint k = i + j;
				if (k >= phaseAlignBufferSize)
					k -= phaseAlignBufferSize;
				correlation[i] += buffer[0][j] * buffer[1][k];
			}
		}

		int polarity = 1;
		int idx = 0;
		double max = 0.;

		for (uint i = 0; i < phaseAlignBufferSize; ++i) // extracting index of max value of cross corr and polarity value
		{
			int pol = 1;
			if (correlation[i] < 0.)
				pol = -1;
			double absCorrelation = correlation[i] * pol;
			if (max < absCorrelation)
			{
				max = absCorrelation;
				idx = int(i);
				polarity = pol;
			}
		}

		if (idx <= bufferSize/2) // determining if a delay is in the left or right audio channel 
			sampleDelay = idx; // First channel is delayed 
		else
			sampleDelay = (phaseAlignBufferSize - idx)*-1; //Second channel is delayed


		outputStrings[0] = sampleDelay;
		outputStrings[1] = polarity;

		state = DetectorState::Off;
	}


}
/** 
*  \file Utils.hxx
*  Common math dsp utils for angelscript.
* 
*  Created by Blue Cat Audio <services@bluecataudio.com>
*  Copyright 2011-2014 Blue Cat Audio. All rights reserved.
*
*/

namespace KittyDSP
{
    namespace Utils
    {
        /** Computes the next power of two for an integer.
        *
        */
        int nextPowerOfTwo(int val) 
        {
            val--;
            val = (val >> 1) | val;
            val = (val >> 2) | val;
            val = (val >> 4) | val;
            val = (val >> 8) | val;
            val = (val >> 16) | val;
            val++;
            if(val==0)
                val=2;
            return val;
        }

        /** convert double number to closest integer (rounding).
        *
        */
        int roundDoubleToInt(double d)
        {
            if(d<0)
                return int(d-.5);
            else
                return int(d+.5);
        }
    }
}
