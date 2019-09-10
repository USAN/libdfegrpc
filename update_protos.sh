#! /bin/bash

if command -v yum > /dev/null ; then
    sudo yum install -y unzip
else
    sudo apt-get install -y unzip
fi

TEMPDIR=`mktemp -d`

GOOGLE_APIS_URL=https://github.com/googleapis/googleapis/archive/master.zip
APIS_FILE=`basename ${GOOGLE_APIS_URL}`

wget "${GOOGLE_APIS_URL}" -O ${TEMPDIR}/${APIS_FILE}

FILES=`unzip -l ${TEMPDIR}/${APIS_FILE} | awk -F' ' '{ print $4; }' | grep -E '(google/api|google/type|google/longrunning|google/rpc|google/logging|google/cloud/texttospeech|google/cloud/dialogflow)' | tr '\n' ' '`

unzip ${TEMPDIR}/${APIS_FILE} -d ${TEMPDIR} ${FILES} 

rm -Rf protos/google/{api, type, longrunning, rpc, logging, cloud/texttospeech, cloud/dialogflow}

mkdir -p protos
tar -C ${TEMPDIR}/googleapis-master -f - -c . | tar -x -v -f - -C protos

rm -Rf ${TEMPDIR}

rm -f Makefile.protos

RULEFILE=`mktemp`
OBJFILE=`mktemp`
PROTOFILE=`mktemp`
CCFILE=`mktemp`

printf 'GRPC_CPP_PLUGIN=$(shell which grpc_cpp_plugin)\n' > ${PROTOFILE}

printf "PROTOS=" >> ${PROTOFILE}
printf "PROTOCCS=" > ${CCFILE}
printf "PROTOOBJS=" > ${OBJFILE}

for f in `find protos -name "*.proto" -print` ; do
    printf " %s" $f >> ${PROTOFILE}
    printf " %s %s" ${f/%proto/pb.cc} ${f/%proto/grpc.pb.cc} >> ${CCFILE}
    printf " %s %s" ${f/%proto/pb.oo} ${f/%proto/grpc.pb.oo} >> ${OBJFILE}
    printf "%s: %s\n" ${f/%proto/pb.cc} ${f} >> $RULEFILE
    printf "\tprotoc --proto_path=protos:/usr/include --cpp_out=protos --plugin=protoc-gen-grpc=\$(GRPC_CPP_PLUGIN) ${f}\n" >> $RULEFILE
    printf "%s: %s\n" ${f/%proto/grpc.pb.cc} ${f} >> $RULEFILE
    printf "\tprotoc --proto_path=protos:/usr/include --grpc_out=generate_mock_code=true:protos --plugin=protoc-gen-grpc=\$(GRPC_CPP_PLUGIN) ${f}\n" >> $RULEFILE
done

printf "\n" >> ${PROTOFILE}
printf "\n" >> ${CCFILE}
printf "\n" >> ${OBJFILE}

cat ${PROTOFILE} ${CCFILE} ${OBJFILE} ${RULEFILE} > Makefile.protos

rm -f ${PROTOFILE} ${CCFILE} ${OBJFILE} ${RULEFILE}
