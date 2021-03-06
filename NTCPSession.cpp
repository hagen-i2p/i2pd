#include <string.h>
#include <stdlib.h>
#include "I2PEndian.h"
#include <cryptopp/dh.h>
#include "base64.h"
#include "Log.h"
#include "Timestamp.h"
#include "CryptoConst.h"
#include "I2NPProtocol.h"
#include "RouterContext.h"
#include "Transports.h"
#include "NetDb.h"
#include "NTCPSession.h"

using namespace i2p::crypto;

namespace i2p
{
namespace transport
{
	NTCPSession::NTCPSession (boost::asio::io_service& service, std::shared_ptr<const i2p::data::RouterInfo> in_RemoteRouter): 
		TransportSession (in_RemoteRouter),	m_Socket (service), 
		m_TerminationTimer (service), m_IsEstablished (false), m_ReceiveBufferOffset (0), 
		m_NextMessage (nullptr), m_NumSentBytes (0), m_NumReceivedBytes (0)
	{		
		m_DHKeysPair = transports.GetNextDHKeysPair ();
		m_Establisher = new Establisher;
	}
	
	NTCPSession::~NTCPSession ()
	{
		delete m_Establisher;
		if (m_NextMessage)	
			i2p::DeleteI2NPMessage (m_NextMessage);
		for (auto it :m_DelayedMessages)
			i2p::DeleteI2NPMessage (it);
		m_DelayedMessages.clear ();	
	}

	void NTCPSession::CreateAESKey (uint8_t * pubKey, i2p::crypto::AESKey& key)
	{
		CryptoPP::DH dh (elgp, elgg);
		uint8_t sharedKey[256];
		if (!dh.Agree (sharedKey, m_DHKeysPair->privateKey, pubKey))
		{    
		    LogPrint (eLogError, "Couldn't create shared key");
			Terminate ();
			return;
		};

		uint8_t * aesKey = key;
		if (sharedKey[0] & 0x80)
		{
			aesKey[0] = 0;
			memcpy (aesKey + 1, sharedKey, 31);
		}	
		else if (sharedKey[0])	
			memcpy (aesKey, sharedKey, 32);
		else
		{
			// find first non-zero byte
			uint8_t * nonZero = sharedKey + 1;
			while (!*nonZero)
			{
				nonZero++;
				if (nonZero - sharedKey > 32)
				{
					LogPrint (eLogWarning, "First 32 bytes of shared key is all zeros. Ignored");
					return;
				}	
			}
			memcpy (aesKey, nonZero, 32);
		}
	}	

	void NTCPSession::Terminate ()
	{
		m_IsEstablished = false;
		m_Socket.close ();
		int numDelayed = 0;
		for (auto it :m_DelayedMessages)
		{	
			// try to send them again
			if (m_RemoteRouter)
				transports.SendMessage (m_RemoteRouter->GetIdentHash (), it);
			numDelayed++;
		}	
		m_DelayedMessages.clear ();
		if (numDelayed > 0)
			LogPrint (eLogWarning, "NTCP session ", numDelayed, " not sent");
		// TODO: notify tunnels
		transports.RemoveNTCPSession (shared_from_this ());
		LogPrint ("NTCP session terminated");
	}	

	void NTCPSession::Connected ()
	{
		m_IsEstablished = true;

		delete m_Establisher;
		m_Establisher = nullptr;
		
		delete m_DHKeysPair;
		m_DHKeysPair = nullptr;	

		SendTimeSyncMessage ();
		SendI2NPMessage (CreateDatabaseStoreMsg ()); // we tell immediately who we are		

		if (!m_DelayedMessages.empty ())
		{
			for (auto it :m_DelayedMessages)
				SendI2NPMessage (it);
			m_DelayedMessages.clear ();
		}	
	}	
		
	void NTCPSession::ClientLogin ()
	{
		if (!m_DHKeysPair)
			m_DHKeysPair = transports.GetNextDHKeysPair ();
		// send Phase1
		const uint8_t * x = m_DHKeysPair->publicKey;
		memcpy (m_Establisher->phase1.pubKey, x, 256);
		CryptoPP::SHA256().CalculateDigest(m_Establisher->phase1.HXxorHI, x, 256);
		const uint8_t * ident = m_RemoteIdentity.GetIdentHash ();
		for (int i = 0; i < 32; i++)
			m_Establisher->phase1.HXxorHI[i] ^= ident[i];
		
		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Establisher->phase1, sizeof (NTCPPhase1)), boost::asio::transfer_all (),
        	std::bind(&NTCPSession::HandlePhase1Sent, shared_from_this (), std::placeholders::_1, std::placeholders::_2));
	}	

	void NTCPSession::ServerLogin ()
	{
		// receive Phase1
		boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Establisher->phase1, sizeof (NTCPPhase1)), boost::asio::transfer_all (),                    
			std::bind(&NTCPSession::HandlePhase1Received, shared_from_this (), 
				std::placeholders::_1, std::placeholders::_2));
	}	
		
	void NTCPSession::HandlePhase1Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint (eLogWarning, "Couldn't send Phase 1 message: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{	
			LogPrint (eLogDebug, "Phase 1 sent: ", bytes_transferred);
			boost::asio::async_read (m_Socket, boost::asio::buffer(&m_Establisher->phase2, sizeof (NTCPPhase2)), boost::asio::transfer_all (),                 
				std::bind(&NTCPSession::HandlePhase2Received, shared_from_this (), 
					std::placeholders::_1, std::placeholders::_2));
		}	
	}	

	void NTCPSession::HandlePhase1Received (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint (eLogError, "Phase 1 read error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{	
			LogPrint (eLogDebug, "Phase 1 received: ", bytes_transferred);
			// verify ident
			uint8_t digest[32];
			CryptoPP::SHA256().CalculateDigest(digest, m_Establisher->phase1.pubKey, 256);
			const uint8_t * ident = i2p::context.GetRouterInfo ().GetIdentHash ();
			for (int i = 0; i < 32; i++)
			{	
				if ((m_Establisher->phase1.HXxorHI[i] ^ ident[i]) != digest[i])
				{
					LogPrint (eLogError, "Wrong ident");
					Terminate ();
					return;
				}	
			}	
			
			SendPhase2 ();
		}	
	}	

	void NTCPSession::SendPhase2 ()
	{
		if (!m_DHKeysPair)
			m_DHKeysPair = transports.GetNextDHKeysPair ();
		const uint8_t * y = m_DHKeysPair->publicKey;
		memcpy (m_Establisher->phase2.pubKey, y, 256);
		uint8_t xy[512];
		memcpy (xy, m_Establisher->phase1.pubKey, 256);
		memcpy (xy + 256, y, 256);
		CryptoPP::SHA256().CalculateDigest(m_Establisher->phase2.encrypted.hxy, xy, 512); 
		uint32_t tsB = htobe32 (i2p::util::GetSecondsSinceEpoch ());
		m_Establisher->phase2.encrypted.timestamp = tsB;
		// TODO: fill filler

		i2p::crypto::AESKey aesKey;
		CreateAESKey (m_Establisher->phase1.pubKey, aesKey);
		m_Encryption.SetKey (aesKey);
		m_Encryption.SetIV (y + 240);
		m_Decryption.SetKey (aesKey);
		m_Decryption.SetIV (m_Establisher->phase1.HXxorHI + 16);
		
		m_Encryption.Encrypt ((uint8_t *)&m_Establisher->phase2.encrypted, sizeof(m_Establisher->phase2.encrypted), (uint8_t *)&m_Establisher->phase2.encrypted);
		boost::asio::async_write (m_Socket, boost::asio::buffer (&m_Establisher->phase2, sizeof (NTCPPhase2)), boost::asio::transfer_all (),
        	std::bind(&NTCPSession::HandlePhase2Sent, shared_from_this (), std::placeholders::_1, std::placeholders::_2, tsB));

	}	
		
	void NTCPSession::HandlePhase2Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsB)
	{
		if (ecode)
        {
			LogPrint (eLogWarning, "Couldn't send Phase 2 message: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{	
			LogPrint (eLogDebug, "Phase 2 sent: ", bytes_transferred);
			boost::asio::async_read (m_Socket, boost::asio::buffer(m_ReceiveBuffer, NTCP_DEFAULT_PHASE3_SIZE), boost::asio::transfer_all (),                   
				std::bind(&NTCPSession::HandlePhase3Received, shared_from_this (), 
					std::placeholders::_1, std::placeholders::_2, tsB));
		}	
	}	
		
	void NTCPSession::HandlePhase2Received (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint ("Phase 2 read error: ", ecode.message (), ". Wrong ident assumed");
			if (ecode != boost::asio::error::operation_aborted)
			{
				// this RI is not valid
				i2p::data::netdb.SetUnreachable (GetRemoteIdentity ().GetIdentHash (), true);
				transports.ReuseDHKeysPair (m_DHKeysPair);
				m_DHKeysPair = nullptr;
				Terminate ();
			}
		}
		else
		{	
			LogPrint (eLogDebug, "Phase 2 received: ", bytes_transferred);
		
			i2p::crypto::AESKey aesKey;
			CreateAESKey (m_Establisher->phase2.pubKey, aesKey);
			m_Decryption.SetKey (aesKey);
			m_Decryption.SetIV (m_Establisher->phase2.pubKey + 240);
			m_Encryption.SetKey (aesKey);
			m_Encryption.SetIV (m_Establisher->phase1.HXxorHI + 16);
			
			m_Decryption.Decrypt((uint8_t *)&m_Establisher->phase2.encrypted, sizeof(m_Establisher->phase2.encrypted), (uint8_t *)&m_Establisher->phase2.encrypted);
			// verify
			uint8_t xy[512], hxy[32];
			memcpy (xy, m_DHKeysPair->publicKey, 256);
			memcpy (xy + 256, m_Establisher->phase2.pubKey, 256);
			CryptoPP::SHA256().CalculateDigest(hxy, xy, 512); 
			if (memcmp (hxy, m_Establisher->phase2.encrypted.hxy, 32))
			{
				LogPrint (eLogError, "Incorrect hash");
				transports.ReuseDHKeysPair (m_DHKeysPair);
				m_DHKeysPair = nullptr;
				Terminate ();
				return ;
			}	
			SendPhase3 ();
		}	
	}	

	void NTCPSession::SendPhase3 ()
	{
		auto keys = i2p::context.GetPrivateKeys ();
		uint8_t * buf = m_ReceiveBuffer; 
		*(uint16_t *)buf = htobe16 (keys.GetPublic ().GetFullLen ());
		buf += 2;
		buf += i2p::context.GetIdentity ().ToBuffer (buf, NTCP_BUFFER_SIZE);
		uint32_t tsA = htobe32 (i2p::util::GetSecondsSinceEpoch ());
		*(uint32_t *)buf = tsA;
		buf += 4;		
		size_t signatureLen = keys.GetPublic ().GetSignatureLen ();
		size_t len = (buf - m_ReceiveBuffer) + signatureLen;
		size_t paddingSize = len & 0x0F; // %16
		if (paddingSize > 0) 
		{
			paddingSize = 16 - paddingSize;
			// TODO: fill padding with random data
			buf += paddingSize;
			len += paddingSize;
		}

		SignedData s;
		s.Insert (m_Establisher->phase1.pubKey, 256); // x
		s.Insert (m_Establisher->phase2.pubKey, 256); // y
		s.Insert (m_RemoteIdentity.GetIdentHash (), 32); // ident
 		s.Insert (tsA);	// tsA
		s.Insert (m_Establisher->phase2.encrypted.timestamp); // tsB
		s.Sign (keys, buf);

		m_Encryption.Encrypt(m_ReceiveBuffer, len, m_ReceiveBuffer);		        
		boost::asio::async_write (m_Socket, boost::asio::buffer (m_ReceiveBuffer, len), boost::asio::transfer_all (),
        	std::bind(&NTCPSession::HandlePhase3Sent, shared_from_this (), std::placeholders::_1, std::placeholders::_2, tsA));				
	}	
		
	void NTCPSession::HandlePhase3Sent (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsA)
	{
		if (ecode)
        {
			LogPrint (eLogWarning, "Couldn't send Phase 3 message: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{	
			LogPrint (eLogDebug, "Phase 3 sent: ", bytes_transferred);
			// wait for phase4 
			auto signatureLen = m_RemoteIdentity.GetSignatureLen ();
			size_t paddingSize = signatureLen & 0x0F; // %16
			if (paddingSize > 0) signatureLen += (16 - paddingSize);	
			boost::asio::async_read (m_Socket, boost::asio::buffer(m_ReceiveBuffer, signatureLen), boost::asio::transfer_all (),                  
				std::bind(&NTCPSession::HandlePhase4Received, shared_from_this (), 
					std::placeholders::_1, std::placeholders::_2, tsA));
		}	
	}	

	void NTCPSession::HandlePhase3Received (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsB)
	{	
		if (ecode)
        {
			LogPrint (eLogError, "Phase 3 read error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{	
			LogPrint (eLogDebug, "Phase 3 received: ", bytes_transferred);
			m_Decryption.Decrypt (m_ReceiveBuffer, bytes_transferred, m_ReceiveBuffer);
			uint8_t * buf = m_ReceiveBuffer;
			uint16_t size = be16toh (*(uint16_t *)buf);
			m_RemoteIdentity.FromBuffer (buf + 2, size);
			size_t expectedSize = size + 2/*size*/ + 4/*timestamp*/ + m_RemoteIdentity.GetSignatureLen ();
			size_t paddingLen = expectedSize & 0x0F;
			if (paddingLen) paddingLen = (16 - paddingLen);	
			if (expectedSize > NTCP_DEFAULT_PHASE3_SIZE)
			{
				// we need more bytes for Phase3
				expectedSize += paddingLen;	
				LogPrint (eLogDebug, "Wait for ", expectedSize, " more bytes for Phase3");
				boost::asio::async_read (m_Socket, boost::asio::buffer(m_ReceiveBuffer + NTCP_DEFAULT_PHASE3_SIZE, expectedSize), boost::asio::transfer_all (),                   
				std::bind(&NTCPSession::HandlePhase3ExtraReceived, shared_from_this (), 
					std::placeholders::_1, std::placeholders::_2, tsB, paddingLen));
			}
			else
				HandlePhase3 (tsB, paddingLen);
		}	
	}

	void NTCPSession::HandlePhase3ExtraReceived (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsB, size_t paddingLen)
	{
		if (ecode)
        {
			LogPrint (eLogError, "Phase 3 extra read error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{
			LogPrint (eLogDebug, "Phase 3 extra received: ", bytes_transferred);
			m_Decryption.Decrypt (m_ReceiveBuffer + NTCP_DEFAULT_PHASE3_SIZE, bytes_transferred, m_ReceiveBuffer+ NTCP_DEFAULT_PHASE3_SIZE);
			HandlePhase3 (tsB, paddingLen);
		}		
	}	

	void NTCPSession::HandlePhase3 (uint32_t tsB, size_t paddingLen)
	{
		uint8_t * buf = m_ReceiveBuffer + m_RemoteIdentity.GetFullLen () + 2 /*size*/;
		uint32_t tsA = *(uint32_t *)buf; 
		buf += 4;
		buf += paddingLen;	

		SignedData s;
		s.Insert (m_Establisher->phase1.pubKey, 256); // x
		s.Insert (m_Establisher->phase2.pubKey, 256); // y
		s.Insert (i2p::context.GetRouterInfo ().GetIdentHash (), 32); // ident
		s.Insert (tsA); // tsA
		s.Insert (tsB); // tsB			
		if (!s.Verify (m_RemoteIdentity, buf))
		{	
			LogPrint (eLogError, "signature verification failed");
			Terminate ();
			return;
		}	

		SendPhase4 (tsA, tsB);
	}

	void NTCPSession::SendPhase4 (uint32_t tsA, uint32_t tsB)
	{
		SignedData s;
		s.Insert (m_Establisher->phase1.pubKey, 256); // x
		s.Insert (m_Establisher->phase2.pubKey, 256); // y
		s.Insert (m_RemoteIdentity.GetIdentHash (), 32); // ident
		s.Insert (tsA); // tsA
		s.Insert (tsB); // tsB
		auto keys = i2p::context.GetPrivateKeys ();
 		auto signatureLen = keys.GetPublic ().GetSignatureLen ();
		s.Sign (keys, m_ReceiveBuffer);
		size_t paddingSize = signatureLen & 0x0F; // %16
		if (paddingSize > 0) signatureLen += (16 - paddingSize);		
		m_Encryption.Encrypt (m_ReceiveBuffer, signatureLen, m_ReceiveBuffer);

		boost::asio::async_write (m_Socket, boost::asio::buffer (m_ReceiveBuffer, signatureLen), boost::asio::transfer_all (),
        	std::bind(&NTCPSession::HandlePhase4Sent, shared_from_this (), std::placeholders::_1, std::placeholders::_2));
	}	

	void NTCPSession::HandlePhase4Sent (const boost::system::error_code& ecode,  std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint (eLogWarning, "Couldn't send Phase 4 message: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{	
			LogPrint (eLogDebug, "Phase 4 sent: ", bytes_transferred);
			LogPrint ("NTCP server session connected");
			transports.AddNTCPSession (shared_from_this ());

			Connected ();
			m_ReceiveBufferOffset = 0;
			m_NextMessage = nullptr;
			Receive ();
		}	
	}	
		
	void NTCPSession::HandlePhase4Received (const boost::system::error_code& ecode, std::size_t bytes_transferred, uint32_t tsA)
	{
		if (ecode)
        {
			LogPrint (eLogError, "Phase 4 read error: ", ecode.message ());
			if (ecode != boost::asio::error::operation_aborted)
			{
				 // this router doesn't like us	
				i2p::data::netdb.SetUnreachable (GetRemoteIdentity ().GetIdentHash (), true);
				Terminate ();
			}	
		}
		else
		{	
			LogPrint (eLogDebug, "Phase 4 received: ", bytes_transferred);
			m_Decryption.Decrypt(m_ReceiveBuffer, bytes_transferred, m_ReceiveBuffer);

			// verify signature
			SignedData s;
			s.Insert (m_Establisher->phase1.pubKey, 256); // x
			s.Insert (m_Establisher->phase2.pubKey, 256); // y
			s.Insert (i2p::context.GetRouterInfo ().GetIdentHash (), 32); // ident
			s.Insert (tsA); // tsA
			s.Insert (m_Establisher->phase2.encrypted.timestamp); // tsB

			if (!s.Verify (m_RemoteIdentity, m_ReceiveBuffer))
			{	
				LogPrint (eLogError, "signature verification failed");
				Terminate ();
				return;
			}	
			LogPrint ("NTCP session connected");
			Connected ();
						
			m_ReceiveBufferOffset = 0;
			m_NextMessage = nullptr;
			Receive ();
		}
	}

	void NTCPSession::Receive ()
	{
		m_Socket.async_read_some (boost::asio::buffer(m_ReceiveBuffer + m_ReceiveBufferOffset, NTCP_BUFFER_SIZE - m_ReceiveBufferOffset),                
			std::bind(&NTCPSession::HandleReceived, shared_from_this (), 
			std::placeholders::_1, std::placeholders::_2));
	}	
		
	void NTCPSession::HandleReceived (const boost::system::error_code& ecode, std::size_t bytes_transferred)
	{
		if (ecode)
        {
			LogPrint (eLogError, "Read error: ", ecode.message ());
			//if (ecode != boost::asio::error::operation_aborted)
				Terminate ();
		}
		else
		{
			m_NumReceivedBytes += bytes_transferred;
			m_ReceiveBufferOffset += bytes_transferred;

			if (m_ReceiveBufferOffset >= 16)
			{	
				uint8_t * nextBlock = m_ReceiveBuffer;
				while (m_ReceiveBufferOffset >= 16)
				{
					if (!DecryptNextBlock (nextBlock)) // 16 bytes
					{
						Terminate ();
						return; 
					}	
					nextBlock += 16;
					m_ReceiveBufferOffset -= 16;
				}	
				if (m_ReceiveBufferOffset > 0)
					memcpy (m_ReceiveBuffer, nextBlock, m_ReceiveBufferOffset);
			}	
			
			ScheduleTermination (); // reset termination timer
			Receive ();
		}	
	}	

	bool NTCPSession::DecryptNextBlock (const uint8_t * encrypted) // 16 bytes
	{
		if (!m_NextMessage) // new message, header expected
		{	
			m_NextMessage = i2p::NewI2NPMessage ();
			m_NextMessageOffset = 0;
			
			m_Decryption.Decrypt (encrypted, m_NextMessage->buf);
			uint16_t dataSize = be16toh (*(uint16_t *)m_NextMessage->buf);
			if (dataSize)
			{
				// new message
				if (dataSize > NTCP_MAX_MESSAGE_SIZE)
				{
					LogPrint (eLogError, "NTCP data size ", dataSize, " exceeds max size");
					i2p::DeleteI2NPMessage (m_NextMessage);
					m_NextMessage = nullptr;
					return false;
				}
				m_NextMessageOffset += 16;
				m_NextMessage->offset = 2; // size field
				m_NextMessage->len = dataSize + 2; 
			}	
			else
			{	
				// timestamp
				LogPrint ("Timestamp");	
				i2p::DeleteI2NPMessage (m_NextMessage);
				m_NextMessage = nullptr;
				return true;
			}	
		}	
		else // message continues
		{	
			m_Decryption.Decrypt (encrypted, m_NextMessage->buf + m_NextMessageOffset);
			m_NextMessageOffset += 16;
		}		
		
		if (m_NextMessageOffset >= m_NextMessage->len + 4) // +checksum
		{	
			// we have a complete I2NP message
			i2p::HandleI2NPMessage (m_NextMessage);	
			m_NextMessage = nullptr;
		}
		return true;	
 	}	

	void NTCPSession::Send (i2p::I2NPMessage * msg)
	{
		uint8_t * sendBuffer;
		int len;

		if (msg)
		{	
			// regular I2NP
			if (msg->offset < 2)
			{
				LogPrint (eLogError, "Malformed I2NP message");
				i2p::DeleteI2NPMessage (msg);
			}	
			sendBuffer = msg->GetBuffer () - 2; 
			len = msg->GetLength ();
			*((uint16_t *)sendBuffer) = htobe16 (len);
		}	
		else
		{
			// prepare timestamp
			sendBuffer = m_TimeSyncBuffer;
			len = 4;
			*((uint16_t *)sendBuffer) = 0;
			*((uint32_t *)(sendBuffer + 2)) = htobe32 (time (0));
		}	
		int rem = (len + 6) & 0x0F; // %16
		int padding = 0;
		if (rem > 0) padding = 16 - rem;
		// TODO: fill padding 
		m_Adler.CalculateDigest (sendBuffer + len + 2 + padding, sendBuffer, len + 2+ padding);

		int l = len + padding + 6;
		m_Encryption.Encrypt(sendBuffer, l, sendBuffer);	

		boost::asio::async_write (m_Socket, boost::asio::buffer (sendBuffer, l), boost::asio::transfer_all (),                      
        	std::bind(&NTCPSession::HandleSent, shared_from_this (), std::placeholders::_1, std::placeholders::_2, msg));	
	}
		
	void NTCPSession::HandleSent (const boost::system::error_code& ecode, std::size_t bytes_transferred, i2p::I2NPMessage * msg)
	{		
		if (msg)
			i2p::DeleteI2NPMessage (msg);
		if (ecode)
        {
			LogPrint (eLogWarning, "Couldn't send msg: ", ecode.message ());
			// we shouldn't call Terminate () here, because HandleReceive takes care
			// TODO: 'delete this' statement in Terminate () must be eliminated later
			// Terminate ();
		}
		else
		{	
			m_NumSentBytes += bytes_transferred;
			ScheduleTermination (); // reset termination timer
		}	
	}

	void NTCPSession::SendTimeSyncMessage ()
	{
		Send (nullptr);
	}	

	void NTCPSession::SendI2NPMessage (I2NPMessage * msg)
	{
		if (msg)
		{
			if (m_IsEstablished)
				Send (msg);
			else
				m_DelayedMessages.push_back (msg);	
		}	
	}	

	void NTCPSession::ScheduleTermination ()
	{
		m_TerminationTimer.cancel ();
		m_TerminationTimer.expires_from_now (boost::posix_time::seconds(NTCP_TERMINATION_TIMEOUT));
		m_TerminationTimer.async_wait (std::bind (&NTCPSession::HandleTerminationTimer,
			shared_from_this (), std::placeholders::_1));
	}

	void NTCPSession::HandleTerminationTimer (const boost::system::error_code& ecode)
	{
		if (ecode != boost::asio::error::operation_aborted)
		{	
			LogPrint ("No activity fo ", NTCP_TERMINATION_TIMEOUT, " seconds");
			//Terminate ();
			m_Socket.close ();// invoke Terminate () from HandleReceive 
		}	
	}	
}	
}	
