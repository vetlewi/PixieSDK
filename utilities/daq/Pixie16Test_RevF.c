/*----------------------------------------------------------------------
* Copyright (c) 2005 - 2020, XIA LLC
* All rights reserved.
*
* Redistribution and use in source and binary forms,
* with or without modification, are permitted provided
* that the following conditions are met:
*
*   * Redistributions of source code must retain the above
*     copyright notice, this list of conditions and the
*     following disclaimer.
*   * Redistributions in binary form must reproduce the
*     above copyright notice, this list of conditions and the
*     following disclaimer in the documentation and/or other
*     materials provided with the distribution.
*   * Neither the name of XIA LLC nor the names of its
*     contributors may be used to endorse or promote
*     products derived from this software without
*     specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND
* CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
* INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
* PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
* ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
* TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
* THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
* SUCH DAMAGE.
*----------------------------------------------------------------------*/
/******************************************************************************
*
* File Name:
*
*		Pixie16Test_RevF.c
*
* Description:
*
*		This file contains the minimal code necessary to acquire data from Pixie modules
*
* $Rev$
* $Id$
******************************************************************************/
#include "pixie16app_export.h"
#include "pixie16sys_export.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <math.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <unistd.h>
/////////////timing stuff//////////////

#include <sys/time.h>

//////////////////////////////////////

using namespace std;

int main(int argc, char* argv[]) {
    unsigned short NumModules;
    unsigned short* PXISlotMap;
    char ComFPGAConfigFile[80];
    char SPFPGAConfigFile[80];
    char TrigFPGAConfigFile[80];
    char DSPCodeFile[80];
    char DSPParFile[80];
    char DSPVarFile[80];
    char ErrMSG[256];
    int retval = 0;
    int count;
    char filnam[80];
    unsigned int nFIFOWords[24];
    unsigned int* lmdata;
    unsigned int mod_numwordsread = 0;
    unsigned short EndOfRunRead;

    //////////////////////timing stuff////////////////////////////
    struct timeval tim;
    double t1 = 0, t2 = 0;
    ////////////////////////////////////////

    ////////read cfg file (cfgPixie16.txt)///////////////////////////////////
    const char config[20] = "cfgPixie16.txt";

    ifstream input;
    char* temp = new char[80];
    input.open(config, ios::in);

    if (input.fail()) {
        cout << "can't open the config file ! " << config << endl << flush;
        return false;
    }

    input >> NumModules;
    cout << "\n\n" << NumModules << " modules, in slots:";
    input.getline(temp, 80);
    PXISlotMap = new unsigned short[NumModules + 1];
    for (int i = 0; i < NumModules; i++) {
        input >> PXISlotMap[i];
        input.getline(temp, 80);
        cout << PXISlotMap[i] << " ";
    }

    //==== This code is necessary if modules are installed in two crates ====//
    //input >> PXISlotMap[NumModules];
    //input.getline (temp, 80);
    //cout << PXISlotMap[NumModules] << " ";

    cout << endl << "Firmware files: \n";
    input >> ComFPGAConfigFile;
    input.getline(temp, 80);
    cout << "ComFPGAConfigFile:  " << ComFPGAConfigFile << endl;
    input >> SPFPGAConfigFile;
    input.getline(temp, 80);
    cout << "SPFPGAConfigFile:   " << SPFPGAConfigFile << endl;
    input >> TrigFPGAConfigFile;
    input.getline(temp, 80);
    cout << "TrigFPGAConfigFile: " << TrigFPGAConfigFile << endl;
    input >> DSPCodeFile;
    input.getline(temp, 80);
    cout << "DSPCodeFile:        " << DSPCodeFile << endl;
    input >> DSPParFile;
    input.getline(temp, 80);
    cout << "DSPParFile:         " << DSPParFile << endl;
    input >> DSPVarFile;
    input.getline(temp, 80);
    cout << "DSPVarFile:         " << DSPVarFile << endl;
    input.close();
    input.clear();

    ////////////////////////////////////////////////////////////////////
    cout << "-----------------------------------------\n";
    cout << "Booting...\n";

    retval = Pixie16InitSystem(NumModules, PXISlotMap, 0);
    if (retval < 0) {
        sprintf(ErrMSG, "*ERROR* Pixie16InitSystem failed, retval = %d", retval);
        Pixie_Print_MSG(ErrMSG);
        return -1;
    } else {
        cout << "Init OK " << retval << endl;
    }

    ///////////////////////////////////////////////////
    // Here we use Boot Pattern 0x70, i.e. only loads the
    // DSP parameters, Program FIPPIS and Set DACs, etc. We assume
    // the Pixie-16 modules have been booted using either nscope or
    // other programs. The truth is we don't need to reboot the
    // Pixie-16 modules each time when we try to acquire data.

    retval = Pixie16BootModule(ComFPGAConfigFile, SPFPGAConfigFile, TrigFPGAConfigFile, DSPCodeFile, DSPParFile,
                               DSPVarFile, NumModules, 0x70);
    if (retval < 0) {
        sprintf(ErrMSG, "*ERROR* Pixie16BootModule failed, retval = %d", retval);
        Pixie_Print_MSG(ErrMSG);
        return -2;
    } else {
        cout << "Boot OK " << retval << endl;
    }

    // Adjust DC-Offsets
    for (int k = 0; k < NumModules; k++) {
        retval = Pixie16AdjustOffsets(k);
        if (retval < 0) {
            sprintf(ErrMSG, "*ERROR* Pixie16AdjustOffsets in module %d failed, retval = %d", k, retval);
            Pixie_Print_MSG(ErrMSG);
            return -6;
        }
    }

    ///////////////////////////////////////////////////
    // Synchronize modules
    int modnum = 0;
    retval = Pixie16WriteSglModPar("SYNCH_WAIT", 1, modnum);
    if (retval < 0)
        cout << "Synch Wait problem " << retval << endl;
    else
        cout << "Synch Wait OK " << retval << endl;

    retval = Pixie16WriteSglModPar("IN_SYNCH", 0, modnum);
    if (retval < 0)
        cout << "In Sync problem " << retval << endl;
    else
        cout << "In Synch OK " << retval << endl;
    //////////////////////////////

    retval = Pixie16StartListModeRun(NumModules, 0x100, NEW_RUN);
    if (retval < 0) {
        sprintf(ErrMSG, "*ERROR* Pixie16StartListModeRun failed, retval = %d", retval);
        Pixie_Print_MSG(ErrMSG);
        return -3;
    } else
        cout << "List Mode started OK " << retval << endl << flush;

    usleep(100000);  //delay for the DSP boot

    ///////////////////timing stuff/////////////////////////////
    gettimeofday(&tim, NULL);
    t1 = tim.tv_sec + (tim.tv_usec / 1000000.0);
    ////////////////////////////////////////////////////////

    if ((lmdata = (unsigned int*) malloc(sizeof(unsigned int) * 131072)) == NULL) {
        printf("failed to allocate memory block lmdata\n");
        return -4;
    }

    // Clear counters to 0 (counters keep track of how many words each module has read)
    for (int k = 0; k < NumModules; k++) { nFIFOWords[k] = 0; }

    //////////////////////////////////////////////
    // Acquiring data: here we terminate the loop
    // when the Director module accumulates more than
    // a given number of words in its external FIFO.
    while (1) {
        for (int k = 0; k < NumModules; k++) {
            EndOfRunRead = 0;
            sprintf(filnam, "lmdata_mod%d.bin", k);

            retval = Pixie16SaveExternalFIFODataToFile(filnam, &mod_numwordsread, k, EndOfRunRead);
            if (retval < 0) {
                sprintf(ErrMSG, "*ERROR* Pixie16SaveExternalFIFODataToFile failed in module %d, retval = %d", k,
                        retval);
                Pixie_Print_MSG(ErrMSG);
                free(lmdata);
                return -5;
            }
            nFIFOWords[k] += mod_numwordsread;
        }
        if (nFIFOWords[0] >= 20000000) {
            sprintf(ErrMSG, "Number of events reached - stopping run");
            Pixie_Print_MSG(ErrMSG);
            cout << "Number of events reached - ending run" << endl << flush;
            break;
        }

        // Check the run status of the Director module (module #0) to see if the run has been stopped.
        // This is possible in a multi-chassis system where modules in one chassis can stop the run
        // in all chassis.
        retval = Pixie16CheckRunStatus(0);
        if (retval == 0) {
            sprintf(ErrMSG, "Run was stopped but number of events are not reached yet");
            Pixie_Print_MSG(ErrMSG);
            cout << "Run was stopped but number of events are not reached yet" << endl << flush;
            break;
        }
    }

    // Stop run in the Director module (module #0) - a SYNC interrupt should be generated
    // to stop run in all modules simultaneously
    retval = Pixie16EndRun(0);

    // Make sure all modules indeed finish their run successfully.
    for (int k = 0; k < NumModules; k++) {
        count = 0;
        do {
            retval = Pixie16CheckRunStatus(k);
            if (retval != 0) {
                EndOfRunRead = 1;
                sprintf(filnam, "lmdata_mod%d.bin", k);
                retval = Pixie16SaveExternalFIFODataToFile(filnam, &mod_numwordsread, k, EndOfRunRead);
                if (retval < 0) {
                    sprintf(ErrMSG, "*ERROR* Pixie16SaveExternalFIFODataToFile failed in module %d, retval = %d", k,
                            retval);
                    Pixie_Print_MSG(ErrMSG);
                    free(lmdata);
                    return -5;
                }
                nFIFOWords[k] += mod_numwordsread;
            } else {
                break;
            }

            count++;
        } while (count < 10);
        if (count == 10) {
            cout << "end run in module " << k << " failed" << endl << flush;
        }
    }

    //////////////////////timing stuff///////////////////////
    gettimeofday(&tim, NULL);
    t2 = tim.tv_sec + (tim.tv_usec / 1000000.0);

    cout << "run time: " << t2 - t1 << endl;
    /////////////////////////////////////////////////////////

    // All modules have their run stopped successfully. Now read out the possible last words from the external FIFO
    for (int k = 0; k < NumModules; k++) {
        EndOfRunRead = 1;
        sprintf(filnam, "lmdata_mod%d.bin", k);
        retval = Pixie16SaveExternalFIFODataToFile(filnam, &mod_numwordsread, k, EndOfRunRead);
        if (retval < 0) {
            sprintf(ErrMSG, "*ERROR* Pixie16SaveExternalFIFODataToFile failed in module %d, retval = %d", k, retval);
            Pixie_Print_MSG(ErrMSG);
            free(lmdata);
            return -5;
        }
        nFIFOWords[k] += mod_numwordsread;
    }

    free(lmdata);

    /////////////////////
    //Save DSP parameters to file
    retval = Pixie16SaveDSPParametersToFile("DSPpar.set");
    if (retval < 0)
        printf("saving DSP parameters to file failed, retval=%d\n", retval);
    else
        cout << "Saving DSP parameters to file OK " << retval << endl << flush;


    // Read histograms from each module and then save them to files
    for (int i = 0; i < NumModules; i++) {
        sprintf(filnam, "histogram_mod%d.bin", i);
        Pixie16SaveHistogramToFile(filnam, i);
    }

    cout << "Run ended \n" << flush;
    exit(1);
}
