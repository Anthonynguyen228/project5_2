/*
CS_4760
Anthony Nguyen
04/19/2022
*/


#ifndef CONFIG_H
#define CONFIG_H


#define processSize 18 //Active processes
#define processMaximum 40 
#define sleeptime 250
#define optionVerbose 1
#define maxRuntime 5 //maximum allowed runtime

#define termCheckPeriod 250 


#define descriptorCount	20 //number of descriptors

#define descriptorValueMax 10 //maximum instances of descriptors

#define sharedDescriptors 20 //percentage of shared descriptors


#define B 30 //Bound parameter used to generate a random number in the range [0, B]

#define lineLimit 100000

#endif
