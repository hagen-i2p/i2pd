#ifndef RESEED_H
#define RESEED_H

#include <iostream>
#include <string>
#include <vector>

namespace i2p
{
namespace data
{

	class Reseeder
	{
		public:
		
			Reseeder();
			~Reseeder();
			bool reseedNow(); // depreacted
			int ReseedNowSU3 ();

			void LoadCertificate (const std::string& filename);
		
		private:

			int ReseedFromSU3 (const std::string& host);
			int ProcessSU3File (const char * filename);	
			int ProcessSU3Stream (std::istream& s);	

			bool FindZipDataDescriptor (std::istream& s);
			
	};
}
}

#endif
