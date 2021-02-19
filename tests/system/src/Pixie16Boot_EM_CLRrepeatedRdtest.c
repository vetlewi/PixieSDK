#include "pixie16app_export.h"
#include "pixie16sys_export.h"
#include "def21160.h"

#include <sstream>
#include <iostream>
#include <fstream>
#include <cstdio>
#include <signal.h>
#include <cstring>
#include <string>

/////////////timing stuff//////////////

#include <sys/time.h>

//////////////////////////////////////

using namespace std;

int main(int argc, char *argv[])
{
	unsigned short NumModules;
	unsigned short *PXISlotMap;
	char ComFPGAConfigFile[80];
	char SPFPGAConfigFile[80];
	char TrigFPGAConfigFile[80];
	char DSPCodeFile[80];
	char DSPParFile[80];
	char DSPVarFile[80];
	char ErrMSG[256];
	int retval=0;
	unsigned int numloops;
	unsigned int errorcounts, rd_wr_errors;	
	unsigned int loopwait;	
	unsigned int rd_data[65536], wr_data[65536];
	unsigned int count;
	unsigned short modnum, emwrites;


	////////read cfg file (cfgPixie16.txt)///////////////////////////////////
	const char config[20]="cfgPixie16.txt";

	ifstream input;
	char *temp = new char[80];
	input.open (config, ios::in);

	if (input.fail ())
	{
		cout << "can't open the config file ! " << config << endl << flush;
		return false;
	}

	input >> NumModules;
	cout << "\n\n" << NumModules << " modules, in slots:";
	input.getline (temp, 80);
	PXISlotMap = new unsigned short[NumModules+1];
	for (int i = 0; i < NumModules; i++)
	{
		input >> PXISlotMap[i];
		input.getline (temp, 80);
		cout << PXISlotMap[i] << " ";
	}

	//==== This code is necessary if modules are installed in two crates ====//
	//input >> PXISlotMap[NumModules];
	//input.getline (temp, 80);
	//cout << PXISlotMap[NumModules] << " ";

	cout << endl << "Firmware files: \n";
	input >> ComFPGAConfigFile;
	input.getline (temp, 80);
	cout << "ComFPGAConfigFile:  " << ComFPGAConfigFile << endl;
	input >> SPFPGAConfigFile;
	input.getline (temp, 80);
	cout << "SPFPGAConfigFile:   " << SPFPGAConfigFile << endl;
	input >> TrigFPGAConfigFile;
	input.getline (temp, 80);
	cout << "TrigFPGAConfigFile: " << TrigFPGAConfigFile << endl;
	input >> DSPCodeFile;
	input.getline (temp, 80);
	cout << "DSPCodeFile:        " << DSPCodeFile << endl;
	input >> DSPParFile;
	input.getline (temp, 80);
	cout << "DSPParFile:         " << DSPParFile << endl;
	input >> DSPVarFile;
	input.getline (temp, 80);
	cout << "DSPVarFile:         " << DSPVarFile << endl;
	input.close();
	input.clear();

	////////////////////////////////////////////////////////////////////
	cout<<"-----------------------------------------\n";
	cout<<"Booting....\n";

	retval = Pixie16InitSystem (NumModules, PXISlotMap, 0);
	if (retval < 0)
	{
		sprintf (ErrMSG, "*ERROR* Pixie16InitSystem failed, retval = %d",
			retval);
		Pixie_Print_MSG (ErrMSG);
		return -1;
	}
	else
	{
		cout<<"Init OK "<<retval<<endl;
	}

	retval = Pixie16BootModule (ComFPGAConfigFile, SPFPGAConfigFile,
		TrigFPGAConfigFile, DSPCodeFile, DSPParFile,
		DSPVarFile, NumModules, 0x7F);
	if (retval < 0)
	{
		sprintf (ErrMSG, "*ERROR* Pixie16BootModule failed, retval = %d", retval);
		Pixie_Print_MSG (ErrMSG);
		return -2;
	}
  		
	sprintf (ErrMSG, "Pixie16BootModule succeeded, retval = %d", retval);
	Pixie_Print_MSG (ErrMSG);
	cout<<"Boot OK "<<retval<<endl;

	// Writre ramp data to EM
	for(modnum = 0; modnum < NumModules; modnum ++)
	{
		retval = Pixie_Clear_Main_Memory(0, 32768 * 16, modnum);
		if (retval < 0)
		{
			sprintf (ErrMSG, "*ERROR* Pixie16_Clear_Main_Memory failed in module %d, retval = %d", modnum, retval);
			Pixie_Print_MSG (ErrMSG);
			return(-1);
		}
	}	

	numloops = 0;
	errorcounts = 0;
	do
	{
		for(modnum = 0; modnum < NumModules; modnum ++)
		{
			rd_wr_errors = 0;

			for(emwrites = 0; emwrites < 8; emwrites ++)
			{
				for(count = 0; count < 65536; count ++)
				{
					wr_data[count] = 0;
					rd_data[count] = 0xA5A5A5A5;
				}

				retval = Pixie_Main_Memory_IO(rd_data, 65536 * emwrites, 65536, MOD_READ, modnum);
				if (retval < 0)
				{
					sprintf (ErrMSG, "*ERROR* Pixie16_Main_Memory_IO READ failed in module %d, retval = %d", modnum, retval);
					Pixie_Print_MSG (ErrMSG);
				}
					
				for(count = 0; count < 65536; count ++)
				{
					if(wr_data[count] != rd_data[count])
					{
						sprintf (ErrMSG, "RD_WR data mismatch in module %d, rd_data=0x%x, wr_data=0x%x, em address=%x", modnum, rd_data[count], wr_data[count], count+emwrites*65536);
						Pixie_Print_MSG (ErrMSG);
						rd_wr_errors ++;

//						return 2;
					}
				}
				if(rd_wr_errors > 0)
				{
					sprintf (ErrMSG, "RD_WR data mismatch in module %d, rd_wr_errors=%d", modnum, rd_wr_errors);
					Pixie_Print_MSG (ErrMSG);
					printf ("RD_WR data mismatch in module %d, rd_wr_errors=%d\n", modnum, rd_wr_errors);

				}
				errorcounts += rd_wr_errors;
			}
		}
		numloops ++;
		printf("Number of EM write & read tests = %d, error counts = %d\n", numloops, errorcounts);
		for(loopwait=0; loopwait<1; loopwait++)
		{
			usleep(100000);
		}
	}while(numloops < 100);
//	}while(1);

	return 1;
}
