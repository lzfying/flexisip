/*
 Flexisip, a flexible SIP proxy server with media capabilities.
 Copyright (C) 2010-2021  Belledonne Communications SARL, All rights reserved.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as
 published by the Free Software Foundation, either version 3 of the
 License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iosfwd>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <sofia-sip/sip.h>
#include <sofia-sip/su_random.h>
#include <sofia-sip/url.h>

#include <flexisip/agent.hh>
#include <flexisip/logmanager.hh>
#include <flexisip/module.hh>
#include <flexisip/push-param.hh>

#include "utils/sip-uri.hh"

namespace flexisip {

class ContactUpdateListener;

struct ExtendedContactCommon {
	std::string mContactId{};
	std::string mCallId{};
	std::string mUniqueId{};
	std::list<std::string> mPath{};

	ExtendedContactCommon(const char *contactId, const std::list<std::string> &path, const std::string &callId,
			const std::string &uniqueId):
		mContactId{contactId},
		mCallId{callId},
		mUniqueId{uniqueId},
		mPath{path} {}

	ExtendedContactCommon(const std::string &route) : mPath{route} {}
};

struct ExtendedContact {
	class Record;
	friend class Record;

	std::string mContactId{};
	std::string mCallId{};
	std::string mUniqueId{};
	std::list<std::string> mPath{}; //list of urls as string (not enclosed with brakets)
	std::string mUserAgent{};
	sip_contact_t *mSipContact{nullptr}; // Full contact
	float mQ{1.0f};
	time_t mExpireAt{std::numeric_limits<time_t>::max()};
	time_t mExpireNotAtMessage{std::numeric_limits<time_t>::max()};  // real expires time but not for message
	time_t mUpdatedTime{0};
	uint32_t mCSeq{0};
	std::list<std::string> mAcceptHeader{};
	uintptr_t mConnId{0}; // a unique id shared with associate t_port
	sofiasip::Home mHome{};
	bool mAlias{false};
	bool mUsedAsRoute{false}; /*whether the contact information shall be used as a route when forming a request, instead of
						  replacing the request-uri*/

	bool mIsFallback = false; // boolean indicating whether this ExtendedContact is a fallback route or not. There is no need for it to be serialized to database.

	PushParamList mPushParamList{};

	const char *callId() const {return mCallId.c_str();}
	const char *line() const {return mUniqueId.c_str();}
	const char *contactId() const {return mContactId.c_str();}
	const char *route() const {return (mPath.empty() ? nullptr : mPath.cbegin()->c_str());}
	const char *userAgent() const {return mUserAgent.c_str();}
	const std::string &getUserAgent() const {return mUserAgent;}

	static int resolveExpire(const char *contact_expire, int global_expire) {
		if (contact_expire) {
			return atoi(contact_expire);
		} else {
			if (global_expire >= 0) {
				return global_expire;
			} else {
				return -1;
			}
		}
	}

	static std::string urlToString(const url_t *url) {
		std::ostringstream ostr;
		sofiasip::Home home;
		char *tmp = url_as_string(home.home(), url);
		return std::string(tmp ? tmp : "");
	}
	//This function ensures compatibility with old redis record where url was stored with brakets.
	static std::string compatUrlToString(const char *url) {
		if (url[0] == '<' && url[1] != '\0'){
			return std::string(url, 1, strlen(url)-2);
		}
		return std::string(url);
	}

	const std::string &getUniqueId() const {
		return (mUniqueId.empty() ? mCallId : mUniqueId);
	}

	time_t getExpireNotAtMessage() const {
		return mExpireNotAtMessage;
	}

	std::string serializeAsUrlEncodedParams();

	std::string getOrgLinphoneSpecs() const;

	void extractInfoFromHeader(const char *urlHeaders);
	const std::string getMessageExpires(const msg_param_t *m_params);
	void init(bool initExpire = true);
	void extractInfoFromUrl(const char* full_url);

	ExtendedContact(const char *contactId, const char *uniqueId, const char* fullUrl)
	:
		mContactId{contactId ? contactId : ""},
		mUniqueId{uniqueId ? uniqueId : ""}
	{
		extractInfoFromUrl(fullUrl);
		init();
	}

	ExtendedContact(const ExtendedContactCommon &common, const sip_contact_t *sip_contact, int global_expire, uint32_t cseq,
					time_t updateTime, bool alias, const std::list<std::string> &acceptHeaders, const std::string &userAgent)
		: mContactId(common.mContactId), mCallId(common.mCallId), mUniqueId(common.mUniqueId), mPath(common.mPath),
			mUserAgent(userAgent), mExpireNotAtMessage(global_expire), mUpdatedTime(updateTime),
			mCSeq(cseq), mAcceptHeader(acceptHeaders), mAlias(alias) {

		mSipContact = sip_contact_dup(mHome.home(), sip_contact);
		mSipContact->m_next = nullptr;
		init();
	}

	/**
	 * Forge an ExtendedContact from a SIP URI. Optionaly, a route and
	 * the 'q' paramter of the Contact may be set.
	 * The new ExtendedConact has the maximum expiration date.
	 */
	ExtendedContact(const SipUri &url, const std::string &route, float q = 1.0) : mPath({route}) {
		mSipContact = sip_contact_create(mHome.home(), reinterpret_cast<const url_string_t *>(url.get()), nullptr);
		q = std::min(1.0f, std::max(0.0f, q)); // force RFC compliance
		mSipContact->m_q = mHome.sprintf("%.3f", q);
		init(false); // MUST be called with [initExpire == false] to keep mExpireAt and mExpireNotAtMessage untouched in
					 // order the contact never expire.
	}

	ExtendedContact(const ExtendedContact &ec)
		: mContactId(ec.mContactId), mCallId(ec.mCallId), mUniqueId(ec.mUniqueId), mPath(ec.mPath), mUserAgent(ec.mUserAgent),
		mSipContact(nullptr), mQ(ec.mQ), mExpireAt(ec.mExpireAt), mExpireNotAtMessage(ec.mExpireNotAtMessage), mUpdatedTime(ec.mUpdatedTime),
		mCSeq(ec.mCSeq), mAcceptHeader(ec.mAcceptHeader), mConnId(ec.mConnId), mHome(), mAlias(ec.mAlias), mUsedAsRoute(ec.mUsedAsRoute), mIsFallback(ec.mIsFallback){
		mSipContact = sip_contact_dup(mHome.home(), ec.mSipContact);
		mSipContact->m_next = nullptr;
	}

	std::ostream &print(std::ostream &stream, time_t _now = getCurrentTime(), time_t offset = 0) const;
	sip_contact_t *toSofiaContact(su_home_t *home, time_t now) const;
	sip_route_t *toSofiaRoute(su_home_t *home) const;

	/*returns a new url_t where ConnId (private flexisip parameter) is removed*/
	url_t *toSofiaUrlClean(su_home_t *home);
};

template <typename TraitsT>
inline std::basic_ostream<char, TraitsT> &operator<<(std::basic_ostream<char, TraitsT> &strm, const ExtendedContact &ec) {
	ec.print(strm);
	return strm;
}

class InvalidAorError : public std::invalid_argument {
public:
	InvalidAorError(const url_t *aor);
	const char *what() const noexcept override {return mAor;}

private:
	sofiasip::Home mHome;
	const char *mAor = nullptr;
};

class Record {
	friend class RegistrarDb;

  private:
	static void init();

	sofiasip::Home mHome;
	std::list<std::shared_ptr<ExtendedContact>> mContacts;
	std::list<std::shared_ptr<ExtendedContact>> mContactsToRemove;
	std::string mKey;
	SipUri mAor;
	bool mIsDomain = false; /*is a domain registration*/
	bool mOnlyStaticContacts = true;

  public:
	static std::list<std::string> sLineFieldNames;
	static int sMaxContacts;
	static bool sAssumeUniqueDomains;

	Record(const SipUri &aor);
	Record(SipUri &&aor);
	Record(const Record &other) = delete; //disable copy constructor, this is unsafe due to su_home_t here.
	Record(Record &&other) = delete; // disable move constructor
	~Record() = default;

	Record &operator=(const Record &other) = delete; //disable assignement operator too
	Record &operator=(Record &&other) = delete; //disable move assignement operator too

	//Get address of record
	const SipUri &getAor() const {return mAor;}

	void insertOrUpdateBinding(const std::shared_ptr<ExtendedContact> &ec, const std::shared_ptr<ContactUpdateListener> &listener);
	const std::shared_ptr<ExtendedContact> extractContactByUniqueId(std::string uid);
	sip_contact_t *getContacts(su_home_t *home, time_t now);
	void pushContact(const std::shared_ptr<ExtendedContact> &ct) {mContacts.push_back(ct);}

	std::list<std::shared_ptr<ExtendedContact>>::iterator removeContact(const std::shared_ptr<ExtendedContact> &ct) {
		return mContacts.erase(find(mContacts.begin(), mContacts.end(), ct));
	}
	bool isInvalidRegister(const std::string &call_id, uint32_t cseq);
	void clean(time_t time, const std::shared_ptr<ContactUpdateListener> &listener);
	void update(const sip_t *sip, int globalExpire, bool alias, int version, const std::shared_ptr<ContactUpdateListener> &listener);
	//Deprecated: this one is used by protobuf serializer
	void update(const ExtendedContactCommon &ecc, const char *sipuri, long int expireAt, float q, uint32_t cseq,
				time_t updated_time, bool alias, const std::list<std::string> accept, bool usedAsRoute,
				const std::shared_ptr<ContactUpdateListener> &listener);
	bool updateFromUrlEncodedParams(const char *key, const char *uid, const char *full_url, const std::shared_ptr<ContactUpdateListener> &listener);

	void print(std::ostream &stream) const;
	bool isEmpty() const {return mContacts.empty();}
	const std::string &getKey() const {return mKey;}
	int count() {return mContacts.size();}
	const std::list<std::shared_ptr<ExtendedContact>> &getExtendedContacts() const {return mContacts;}
	const std::list<std::shared_ptr<ExtendedContact>> &getContactsToRemove() const {return mContactsToRemove;}
	void cleanContactsToRemoveList() {mContactsToRemove.clear();}

	/*
	 * Synthetise the pub-gruu address from an extended contact belonging to this Record.
	 * FIXME: of course this method should be directly attached to ExtendedContact.
	 * Unfortunately, because pub-gruu were not contained in the ExtendedContact, it shall remain in Record for compatibility.
	 */
	url_t *getPubGruu(const std::shared_ptr<ExtendedContact> &ec, su_home_t *home);
	/**
	 * Check if the contacts list size is < to max aor config option and remove older contacts to match restriction if needed
	 */
	void applyMaxAor();
	static int getMaxContacts() {
		if (sMaxContacts == -1)
			init();
		return sMaxContacts;
	}
	time_t latestExpire() const;
	time_t latestExpire(Agent *ag) const;
	static std::list<std::string> route_to_stl(const sip_route_s *route);
	void appendContactsFrom(const std::shared_ptr<Record> &src);

	// A null pointer or an empty AOR leads to an empty key.
	static std::string defineKeyFromUrl(const url_t *aor);
	static SipUri makeUrlFromKey(const std::string &key);
	static std::string extractUniqueId(const sip_contact_t *contact);

	bool haveOnlyStaticContacts() const {return mOnlyStaticContacts;}
};

template <typename TraitsT>
inline std::basic_ostream<char, TraitsT> &operator<<(std::basic_ostream<char, TraitsT> &strm, const Record &record) {
	record.print(strm);
	return strm;
}

/**
 * @brief Interface for RegistrarDB listeners.
 */
class RegistrarDbListener : public StatFinishListener {
  public:
	virtual ~RegistrarDbListener();

	/**
	 * @brief Method called when searching for
	 * a record matching a given SIP identity is completed.
	 * @param[in] r The found record or nullptr if no record
	 * could be found. If not null, the ownership on the object
	 * is held by the implementation and the object might be
	 * destroyed immediately after onRecordFound() has returned.
	 */
	virtual void onRecordFound(const std::shared_ptr<Record> &r) = 0;
	virtual void onError() = 0;
	virtual void onInvalid() = 0;
};

class RegistrarDbStateListener {
public:
	virtual void onRegistrarDbWritable (bool writable) = 0;
};

class ContactUpdateListener : public RegistrarDbListener {
	public:
	virtual ~ContactUpdateListener();
	virtual void onContactUpdated(const std::shared_ptr<ExtendedContact> &ec) = 0;
};

class ListContactUpdateListener {
	public:
	virtual ~ListContactUpdateListener() = default;
	virtual void onContactsUpdated() = 0;

	std::vector<std::shared_ptr<Record>> records;
};

class ContactRegisteredListener {
  public:
	virtual ~ContactRegisteredListener();
	virtual void onContactRegistered(const std::shared_ptr<Record> &r, const std::string &uid) = 0;
};

class LocalRegExpireListener {
public:
	virtual ~LocalRegExpireListener();
	virtual void onLocalRegExpireUpdated(unsigned int count) = 0;
};

struct BindingParameters {
	bool alias;
	bool withGruu;
	int globalExpire;
	int version;
	std::string callId;
	std::string path;
	std::string userAgent;

	BindingParameters() {
		alias = false;
		withGruu = false;
		globalExpire = 0;
		version = 0;
		callId = "";
		path = "";
		userAgent = "";
	}
};

/**
 * A singleton class which holds records contact addresses associated with a from.
 * Both local and remote storage implementations exist.
 * It is used by the Registrar module.
 **/
class RegistrarDb {
	friend class ModuleRegistrar;

  public:
	virtual ~RegistrarDb();
	/**
	 * Reset RegistrarDb::sUnique
	 * WARNING : this method is ONLY there for testing purpose
	 */
	static void resetDB();
	static RegistrarDb *initialize(Agent *ag);
	static RegistrarDb *get();
	void bind(const MsgSip &sipMsg, const BindingParameters &parameter, const std::shared_ptr<ContactUpdateListener> &listener);
	void bind(const SipUri &from, const sip_contact_t *contact, const BindingParameters &parameter, const std::shared_ptr<ContactUpdateListener> &listener);
	void clear(const MsgSip &sip, const std::shared_ptr<ContactUpdateListener> &listener);
	void fetch(const SipUri &url, const std::shared_ptr<ContactUpdateListener> &listener, bool recursive = false);
	void fetch(const SipUri &url, const std::shared_ptr<ContactUpdateListener> &listener, bool includingDomains, bool recursive);
	void fetchList(const std::vector<SipUri > urls, const std::shared_ptr<ListContactUpdateListener> &listener);
	void notifyContactListener (const std::shared_ptr<Record> &r /*might be empty record*/, const std::string &uid);
	void updateRemoteExpireTime(const std::string &key, time_t expireat);
	unsigned long countLocalActiveRecords() {
		return mLocalRegExpire->countActives();
	}

	void addStateListener (const std::shared_ptr<RegistrarDbStateListener> &listener);
	void removeStateListener (const std::shared_ptr<RegistrarDbStateListener> &listener);
	bool isWritable () const { return mWritable; }
	void subscribe(const SipUri &url, const std::shared_ptr<ContactRegisteredListener> &listener);
	/* Returns true if bindings can create a pub-gruu address (when supported by the registering client)*/
	bool gruuEnabled() const { return mGruuEnabled;}; 
	virtual void subscribe(const std::string &topic, const std::shared_ptr<ContactRegisteredListener> &listener);
	virtual void unsubscribe(const std::string &topic, const std::shared_ptr<ContactRegisteredListener> &listener);
	virtual void publish(const std::string &topic, const std::string &uid) = 0;
	bool useGlobalDomain()const{
		return mUseGlobalDomain;
	}
	const std::string &messageExpiresName() {
		return mMessageExpiresName;
	}
	const std::string getMessageExpires(const msg_param_t *m_params);

	void subscribeLocalRegExpire(LocalRegExpireListener *listener) {
		mLocalRegExpire->subscribe(listener);
	}
	void unsubscribeLocalRegExpire(LocalRegExpireListener *listener) {
		mLocalRegExpire->unsubscribe(listener);
	}
	/* Synthesize the pub-gruu SIP URI corresponding to a REGISTER message. +sip.instance is expected in the Contact header.*/
	url_t *synthesizePubGruu(su_home_t *home, const MsgSip &sipMsg);
	
	void getLocalRegisteredAors(std::list<std::string> & aors) const{
		mLocalRegExpire->getRegisteredAors(aors);
	}

  protected:
	class LocalRegExpire {
		std::map<std::string, time_t> mRegMap;
		mutable std::mutex mMutex;
		std::list<LocalRegExpireListener *> mLocalRegListenerList;
		Agent *mAgent;

	  public:
		void remove(const std::string key) {
			std::lock_guard<std::mutex> lock(mMutex);
			mRegMap.erase(key);
		}
		void update(const std::shared_ptr<Record> &record);
		size_t countActives();
		void removeExpiredBefore(time_t before);
		LocalRegExpire(Agent *ag);
		void clearAll() {
			std::lock_guard<std::mutex> lock(mMutex);
			mRegMap.clear();
		}
		void getRegisteredAors(std::list<std::string> &aors)const;

		void subscribe(LocalRegExpireListener *listener);
		void unsubscribe(LocalRegExpireListener *listener);
		void notifyLocalRegExpireListener(unsigned int count);
	};
	virtual void doBind(const MsgSip &sip, int globalExpire, bool alias, int version, const std::shared_ptr<ContactUpdateListener> &listener) = 0;
	virtual void doClear(const MsgSip &sip, const std::shared_ptr<ContactUpdateListener> &listener) = 0;
	virtual void doFetch(const SipUri &url, const std::shared_ptr<ContactUpdateListener> &listener) = 0;
	virtual void doFetchInstance(const SipUri &url, const std::string &uniqueId, const std::shared_ptr<ContactUpdateListener> &listener) = 0;
	virtual void doMigration() = 0;

	int countSipContacts(const sip_contact_t *contact);
	bool errorOnTooMuchContactInBind(const sip_contact_t *sip_contact, const std::string &key,
									 const std::shared_ptr<RegistrarDbListener> &listener);
	void fetchWithDomain(const SipUri &url, const std::shared_ptr<ContactUpdateListener> &listener, bool recursive);
	void notifyContactListener(const std::string &key, const std::string &uid);
	void notifyStateListener () const;

	RegistrarDb(Agent *ag);
	std::multimap<std::string, std::shared_ptr<ContactRegisteredListener>> mContactListenersMap;
	std::list<std::shared_ptr<RegistrarDbStateListener>> mStateListeners;
	LocalRegExpire *mLocalRegExpire;
	std::string mMessageExpiresName;
	static std::unique_ptr<RegistrarDb> sUnique;
	Agent *mAgent;
	bool mWritable = false;
	bool mUseGlobalDomain;
	bool mGruuEnabled;
};

} // namespace flexisip
