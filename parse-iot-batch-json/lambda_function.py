import json
import boto3
import argparse
import time
from botocore.config import Config



def lambda_handler(event, context):

    session = boto3.Session()
    write_client = session.client('timestream-write', config=Config(read_timeout=20, max_pool_connections=5000, retries={'max_attempts': 10}))
    
    current_time = round(time.time() * 1000)

    device = event["client"]
    
    dimensions = [
        {'Name': 'Device', 'Value': device}
    ]
    records = []
    for i in event["data"]:
        temp = {
            'Dimensions': dimensions,
            'MeasureName': i["id"],
            'MeasureValue': str(i["value"]),
            'MeasureValueType': 'DOUBLE',
            'Time': str(current_time + i["time"]*1000)
        }
        records.append(temp)
        
    response = write_client.write_records(DatabaseName="IoT-DB", TableName="Batch-Sensors", Records=records, CommonAttributes={})

    return response